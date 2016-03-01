/**
 * @file y4mpipe.c
 * @brief VLC video filter for accessing external filters via y4m pipes
 * @author Darrell Walisser
 *
 * @license
    The MIT License (MIT)

    Copyright (c) 2016 Darrell Walisser (my.name@gmail.com)

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
 */

#include <stdlib.h>
#include <assert.h>
#include <math.h>
#include <errno.h>

// VLC core
#include <vlc_config.h>
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>
#include <vlc_picture_fifo.h>
#include <vlc_threads.h>

// use glib for process/pipe management (theoretically cross-platform)
#include <glib.h>

// module entry points
static int  y4m_open(vlc_object_t*);
static void y4m_close(vlc_object_t*);

// module descriptor
vlc_module_begin()

    set_shortname("Y4M Pipe")
    set_description("YUV4MPEG Pipe Video Filter")
    set_capability("video filter2", 0)
    set_callbacks(y4m_open, y4m_close)
    set_category(CAT_VIDEO)
    set_category(SUBCAT_VIDEO_VFILTER)

    add_string("y4m-cmd", "cat", "Command", "Command line for y4m consumer/producer", false)
    add_bool("y4m-echo", false, "Echo", "Do not read from command stdout, only output to its stdin", true)

vlc_module_end()


// table of y4m chromas with vlc equivalents
static struct {
    const char* name;
    vlc_fourcc_t fourcc;
} sChromaTable[] = {
    {"410", VLC_CODEC_I410 },
    {"411", VLC_CODEC_I411 },
    {"420", VLC_CODEC_I420 },
    {"440", VLC_CODEC_I440 },
    {"422", VLC_CODEC_I422 },
    {"444", VLC_CODEC_I444 },
    {"420jpeg", VLC_CODEC_J420 },
    {"422jpeg", VLC_CODEC_J422 },
    {"444jpeg", VLC_CODEC_J444 },
    {"440jpeg", VLC_CODEC_J440 },
    { NULL, 0 }
};

/**
 * @class filter_sys_t
 * @brief container for our filter's private data
 */
struct filter_sys_t
{
    char* cmd;  // command line
    bool echo;  // if true do not read from process stdout, just write to stdin

    // process id and pipe file descriptors
    GPid childPid;
    gint stdin, stdout;

    bool startFailed, threadExit;
    
    picture_fifo_t* inputFifo, *outputFifo;

    vlc_cond_t inputCond, outputCond;
    vlc_mutex_t inputMutex, outputMutex;
    vlc_thread_t inputThread, outputThread;

    video_format_t outFormat;

    bool gotFirstOutput;

    // number of buffered pictures on input and output side
    int bufferedIn;
    int bufferedOut;

    // ratio of out to input frames, must be integer. e.g. value of
    // 2 means there is one input frame for every 2 output frames
    int bufferRatio;

    // minimum number of buffered pictures to prevent running out of output
    int minBuffered;

    // total number of frames passed out of filter (debugging)
    int numFrames;

    // last date seen in filter input pictures, used to get the delta
    // between successive pictures
    mtime_t lastDate;
};

/**
 * @brief Write y4m header based on filter in_fmt, or failing that, picture
 * @return number of bytes written or < 0 on error
 */
static int writeY4mHeader(const filter_t* intf, const picture_t* pic, int fd)
{
    const video_format_t* fmt = &intf->fmt_in.video;

    const char* y4mChroma = NULL;
    int i = 0;
    while (sChromaTable[i].name)
    {
        if (sChromaTable[i].fourcc == fmt->i_chroma)
        {
            y4mChroma = sChromaTable[i].name;
            break;
        }
        i++;
    }

    if (!y4mChroma)
    {
        msg_Err(intf, "writeY4mHeader: unsupported chroma: %s",
            vlc_fourcc_GetDescription(VIDEO_ES, fmt->i_chroma));
        return -1;
    }

    msg_Info(intf, "writeY4mHeader: infps=%.4f outfps=%.4f, frameFps=%.4f",
        (float)fmt->i_frame_rate / fmt->i_frame_rate_base,
        (float)intf->fmt_out.video.i_frame_rate / intf->fmt_out.video.i_frame_rate_base,
        (float)pic->format.i_frame_rate / pic->format.i_frame_rate_base);

    int fpsNum = fmt->i_frame_rate;
    int fpsDen = fmt->i_frame_rate_base;

    if (fpsNum == 0 || fpsDen == 0)
    {
        fpsNum = pic->format.i_frame_rate;
        fpsDen = pic->format.i_frame_rate_base;
    }

    if (fpsNum == 0 || fpsDen == 0)
    {
        // some wmv's don't supply a frame rate via VLC asf demuxer
        msg_Err(intf, "writeY4mHeader: vlc didn't supply an input frame rate");
        return -2;
    }

    if (100 < (fpsNum / fpsDen))
    {
        // the ffmpeg asf demuxer might return a bogus frame rate
        msg_Err(intf, "writeY4mHeader: vlc says fps > 100, probably bullshit");
        return -3;
    }

    char header[256];
    sprintf(header,
            "YUV4MPEG2 W%d H%d F%d:%d Ip A%d:%d C%s XVLCPIPE\x0a",
            fmt->i_visible_width,
            fmt->i_visible_height,
            fpsNum,
            fpsDen,
            fmt->i_sar_num,
            fmt->i_sar_den,
            y4mChroma);

    msg_Info(intf, "y4m header: %s", header);

    return write(fd, header, strlen(header));
}

/**
 * @brief Read line using read(), similar to fgets()
 * @return number of bytes read, < 0 if there was error or no newline
 */
static int readLine(char* buffer, int maxLen, int fd)
{
    for (int i = 0; i < maxLen; i++)
    {
        char ch;

        if (1 != read(fd, &ch, 1))
            return -1;

        buffer[i] = ch;
        if (ch == 0xa)
            return i+1;
    }

    return -2;
}

/**
 * @brief Read header of y4m stream and make a compatible video_format_t
 * @return 1 if successful, <=0 on error
 */
static int readY4mHeader(const filter_t* intf, video_format_t* fmt, int fd)
{
    char header[256] = { 0 };
    if (0 >= readLine(header, sizeof(header)-1, fd))
    {
        msg_Err(intf, "readY4mHeader: failed to read header: errno=%d %s",
            errno, strerror(errno));
        return -1;
    }

    if (strncmp(header, "YUV4MPEG2", 9) != 0)
    {
        msg_Err(intf, "readY4mHeader: input is not a y4m stream");
        return -2;
    }

    int width, height, fpsNum, fpsDen, sarNum, sarDen;
    width = height = fpsNum = sarNum = 0;
    fpsDen = sarDen = 1;
    char y4mChroma[32] = { 0 };

    char* ptr = header;
    while (*ptr)
    {
        if (*ptr++ == ' ')
        {
            char field = *ptr++;
            switch (field)
            {
            case 'W':
                sscanf(ptr, "%d", &width);
                break;
            case 'H':
                sscanf(ptr, "%d", &height);
                break;
            case 'F':
                sscanf(ptr, "%d:%d", &fpsNum, &fpsDen);
                break;
            case 'A':
                sscanf(ptr, "%d:%d", &sarNum, &sarDen);
                break;
            case 'C':
                sscanf(ptr, "%31s", y4mChroma);
                break;
            case 'I':
                break;
            }
        }
    }

    vlc_fourcc_t chroma = 0;
    int i = 0;
    while (sChromaTable[i].name)
    {
        if (0 == strcmp(y4mChroma, sChromaTable[i].name))
        {
            chroma = sChromaTable[i].fourcc;
            break;
        }
        i++;
    }

    if (chroma == 0)
    {
        msg_Err(intf, "readY4mHeader: unsupported Y4M chroma: %s", y4mChroma);
        return -3;
    }

    // vspipe is giving aspect 0:0, (maybe a bug), assume 1:1
    // fixme: keep same as the input sar
    if (sarNum == 0 || sarDen == 0)
    {
        msg_Info(intf, "readY4mHeader: assume sar 1:1");
        sarNum = 1;
        sarDen = 1;
    }

    msg_Info(intf, "readY4mHeader: %s", header);
    msg_Info(intf,
             "readY4mHeader: w=%d h=%d fps=%d:%d sar=%d:%d chroma=%s",
             width,
             height,
             fpsNum,
             fpsDen,
             sarNum,
             sarDen,
             y4mChroma);

    video_format_Setup(fmt, chroma, width, height, width, height, sarNum, sarDen);

    fmt->i_frame_rate = fpsNum;
    fmt->i_frame_rate_base = fpsDen;

    return 1;
}

/**
 * @brief Write vlc picture to y4m frame
 * @return 1 on success, <=0 if error
 */
static int writeY4mFrame(const filter_t* intf, const picture_t* src, int fd)
{
    const char* frameHeader = "FRAME\x0a";

    int err = write(fd, frameHeader, strlen(frameHeader));
    if (err <= 0)
    {
        msg_Err(intf, "writeY4mFrame: failed to write frame header");
        return err;
    }

    for (int i = 0; i < src->i_planes; i++)
    {
        const plane_t* plane = &src->p[i];
        const uint8_t* pixels = plane->p_pixels;

//        msg_Info(intf, "write plane %d visible_pitch=%d pitch=%d lines=%d visible_lines=%d",
//            i,
//            plane->i_visible_pitch,
//            plane->i_pitch,
//            plane->i_lines,
//            plane->i_visible_lines);

        // y4m does not align rows, must write each row
        for (int j = 0; j < plane->i_visible_lines; j++)
        {
            err = write(fd, pixels, plane->i_visible_pitch);
            pixels += plane->i_pitch;

            if (err <= 0)
            {
                msg_Err(intf, "writeY4mFrame: failed to write plane");
                return err;
            }
        }
    }

    return 1;
}

/**
 * @brief Read y4m frame into vlc picture
 * @return 1 on success, <=0 on error
 */
static int readY4mFrame(const filter_t* intf, picture_t* dst, int fd)
{
    char header[32] = { 0 };
    int err = readLine(header, sizeof(header) - 1, fd);
    if (err <= 0)
    {
        msg_Err(intf, "readY4mFrame: read frame header failed: err=%d errno=%d %s",
            err, errno, strerror(errno));
        msg_Err(intf, "readY4mFrame: header=%s", header);

        return -1;
    }

    //msg_Info(intf, "readY4mFrame: header=%s", header);

    for (int i = 0; i < dst->i_planes; i++)
    {
        const plane_t* plane = &dst->p[i];
        uint8_t* pixels = plane->p_pixels;

        //msg_Info(intf, "read plane %d lines=%d visible_lines=%d",
        //   i, plane->i_lines, plane->i_visible_lines);

        for (int j = 0; j < plane->i_visible_lines; j++)
        {
            // read() on a pipe will come up short sometimes
            int len = plane->i_visible_pitch;
            uint8_t *ptr = pixels;
            while (len > 0)
            {
                int bytes = read(fd, ptr, len);
                if (bytes <= 0)
                {
                    msg_Err(intf, "readY4mFrame: read plane failed: row %d, bytes %d of %d, errno=%d %s",
                        j, bytes, plane->i_visible_pitch, errno, strerror(errno));
                    return -1;
                }

                ptr += bytes;
                len -= bytes;
            }
            
            pixels += plane->i_pitch;
        }
    }

    return 1;
}

/**
 * @brief Thread writing frames to the subprocess
 */
static void* inputThread(void* userData)
{
    filter_t*    intf = (filter_t*)userData;
    filter_sys_t* sys = intf->p_sys;

    msg_Info(intf, "inputThread: enter");

    while (true)
    {
        picture_t* pic;

        // acquire a frame pushed from y4m_filter()
        vlc_mutex_lock(&sys->inputMutex);
        mutex_cleanup_push(&sys->inputMutex);

        while (NULL == (pic = picture_fifo_Pop(sys->inputFifo)))
        {
            //msg_Info(intf, "inputThread: wait picture");
            vlc_cond_wait(&sys->inputCond, &sys->inputMutex);
        }

        sys->bufferedIn--;

        vlc_mutex_unlock(&sys->inputMutex);
        vlc_cleanup_pop();

        //msg_Info(intf, "inputThread: write picture");

        if (1 != writeY4mFrame(intf, pic, sys->stdin))
        {
            msg_Err(intf, "inputThread: Failed to write y4m frame");
            break;
        }

        picture_Release(pic);
    }

    msg_Info(intf, "inputThread: exit");

    sys->threadExit = true;

    return userData;
}

/**
 * @brief Thread reading frames from the subprocess
 */
static void* outputThread(void* userData)
{
    filter_t*    intf = (filter_t*)userData;
    filter_sys_t* sys = intf->p_sys;

    msg_Info(intf, "outputThread: enter");

    bool gotHeader = false;

    while (true)
    {
        if (!gotHeader)
        {
            video_format_Init(&sys->outFormat, VLC_CODEC_I420);
            if (1 != readY4mHeader(intf, &sys->outFormat, sys->stdout))
                break;
            gotHeader = true;
        }

        picture_t* outPic = picture_NewFromFormat(&sys->outFormat);
        if(1 != readY4mFrame(intf, outPic, sys->stdout))
        {
            picture_Release(outPic);
            break;
        }

        //msg_Info(intf, "outputThread: read picture");

        // fixme: deinterlace filter does this, not sure if we need to;
        // y4m header contains this information
        outPic->b_progressive = true;
        outPic->i_nb_fields = 2;

        picture_fifo_Push(sys->outputFifo, outPic);

        vlc_cond_signal(&sys->outputCond);
    }

    msg_Info(intf, "outputThread: exit");

    sys->threadExit = true;
    vlc_cond_signal(&sys->outputCond);

    return userData;
}

/**
 * @brief Trigger subprocess exit by closing pipes
 */
static void stopProcess(filter_t* intf)
{
    filter_sys_t* sys = intf->p_sys;

    if (0 <= sys->stdin)
    {
        close(sys->stdin);
        if (0 <= sys->stdout)
            close(sys->stdout);

        sys->stdin = -1;
        sys->stdout = -1;

        g_spawn_close_pid(sys->childPid);
        sys->childPid = 0;
    }
}

/**
 * @brief Start subprocess that will do the actual filtering
 * @bool true if the start was successful
 */
static bool startProcess(filter_t* intf)
{
    filter_sys_t* sys = intf->p_sys;

    bool ok = false;
    sys->childPid = 0;
    sys->stdin = -1;
    sys->stdout = -1;

    gchar** argv;
    GError* error = NULL;

    if (!g_shell_parse_argv(sys->cmd, NULL, &argv, &error))
    {
        msg_Err(intf, "startProcess: failed to parse command line");
        return false;
    }

    const gchar* workingDir = NULL;
    gchar** envp = NULL;
    GSpawnFlags flags = G_SPAWN_SEARCH_PATH;
    GSpawnChildSetupFunc childSetup = NULL;
    gpointer userData = NULL;

    if (!g_spawn_async_with_pipes(workingDir,
                                 argv,
                                 envp,
                                 flags,
                                 childSetup,
                                 userData,
                                 &sys->childPid,
                                 &sys->stdin,
                                 &sys->stdout,
                                 NULL,
                                 &error))
    {
        // todo: check gerror, print more specific error message if possible
        msg_Err(intf, "startProcess: failed to start");
    }
    else
    {
        ok = true;
        msg_Info(intf, "startProcess: started pid=%d stdin=%d stdout=%d",
            sys->childPid, sys->stdin, sys->stdout);

        // start input/writer and output/reader threads
        // we want them to respond as soon as possible so use a high priority
        int err = vlc_clone(&sys->inputThread, inputThread, intf, VLC_THREAD_PRIORITY_OUTPUT);

        if (err == VLC_SUCCESS && !sys->echo)
            err = vlc_clone(&sys->outputThread, outputThread, intf, VLC_THREAD_PRIORITY_OUTPUT);

        if (err != VLC_SUCCESS)
        {
            msg_Err(intf, "vlc_clone failed");
            stopProcess(intf);
            ok = false;
        }
    }

    g_strfreev(argv);

    return ok;
}

/**
 * @brief Block for output when we believe it is imminent
 */
static void waitForOutput(filter_t* intf)
{
    filter_sys_t* sys = intf->p_sys;

    msg_Info(intf, "enter wait for output: buffers:%d:%d:%d",
        sys->bufferedIn, sys->bufferedOut, sys->minBuffered);

    // there should be output at some point when there is an
    // extra picture in the input fifo, wait for it to occur
    // if the process exits during this wait threadExit will be set
    // FIXME: a hang is possible in this wait, hard to reproduce
    picture_t* tmp;

    while (NULL == (tmp = picture_fifo_Peek(sys->outputFifo)) && !sys->threadExit)
        vlc_cond_wait(&sys->outputCond, &sys->outputMutex);

    if (tmp)
        picture_Release(tmp);

    msg_Info(intf, "exit wait for output:  buffers:%d:%d:%d",
        sys->bufferedIn, sys->bufferedOut, sys->minBuffered);
}

/**
 * @brief VLC filter callback
 * @return picture(s) containing the filtered frames
 */
 static picture_t* y4m_filter(filter_t* intf, picture_t* srcPic)
{
    filter_sys_t* sys = intf->p_sys;

    //msg_Info(intf, ">>>> filter");

    if (!srcPic)
    {
        //msg_Info(intf, ">>> filter: NULL INPUT");
        return NULL;
    }

    // will be stored to sys->lastDate on return
    mtime_t currDate = srcPic->date;


    // if there was a problem with the subprocess then send back
    // the picture unmodified, at least we won't freeze up vlc
    if (sys->startFailed || sys->threadExit)
        goto ECHO_RETURN;


    // start subprocess and write the y4m header
    // fixme: this can go in open() if fmt_in matches the srcPic
    if (!sys->startFailed && !sys->childPid)
    {
        sys->startFailed = !startProcess(intf);
        if (sys->startFailed)
            goto ECHO_RETURN;

        if (0 >= writeY4mHeader(intf, srcPic, sys->stdin))
        {
            msg_Err(intf, "writeY4mHeader failed: errno=%d %s",
                errno, strerror(errno));

            stopProcess(intf);
            sys->startFailed = true;

            goto ECHO_RETURN;
        }
    }

    //
    // control the buffering level by monitoring the input/output fifos
    //
    // the input/output fifos are emptied/filled by input/output threads
    // in response to the subprocess reading/writing frames
    //
    // if the input fifo is empty, then probably the subprocess wants
    // more input, so we should buffer more input
    //
    // if the output fifo is empty, and the input fifo is not empty, then
    // probably the subprocess is about to write out a frame and we
    // can wait for it to arrive. in practice, most of the time there
    // is no waiting needed, unless the filter is too slow to keep up.
    //
    bool inputEmpty = true;
    bool outputEmpty = true;

    picture_t* tmp = picture_fifo_Peek(sys->inputFifo);
    if (tmp)
    {
        picture_Release(tmp);
        inputEmpty = false;
    }

    tmp = picture_fifo_Peek(sys->outputFifo);
    if (tmp)
    {
        picture_Release(tmp);
        outputEmpty = false;
    }

    // copy picture to input fifo, we can't use picture_Hold or else
    // the decoder or vout would run out of pictures in its pool
    picture_t* inPic = picture_NewFromFormat(&srcPic->format);
    picture_Copy(inPic, srcPic);
    picture_fifo_Push(sys->inputFifo, inPic);

    // signal input thread to wake up and write some more data out
    vlc_mutex_lock(&sys->inputMutex);
    sys->bufferedIn++;
    vlc_cond_signal(&sys->inputCond);
    vlc_mutex_unlock(&sys->inputMutex);

    // if echo is enabled, we're done
    // todo: there should be a limiter on the input buffering in case
    // the subprocess can't keep up
    if (sys->echo)
        goto ECHO_RETURN;


    // keeps track of the number of buffered output pictures, assumes
    // an integer ratio
    // fixme: needs modification to support non-integer ratios
    // and ratios < 1
    sys->bufferedOut += sys->bufferRatio;

    // handle buffering
    if (outputEmpty && inputEmpty)
    {
        // we haven't supplied enough input, raise the minimum
        // level of buffer to keep and return
        sys->minBuffered += sys->bufferRatio;

        msg_Info(intf, "buffer more input: buffers:%d:%d:%d",
                sys->bufferedIn, sys->bufferedOut, sys->minBuffered);

        goto NULL_RETURN;
    }

    if (outputEmpty)
        waitForOutput(intf);

    // if we don't know what the frame interval is, make it 0 which
    // probably causes the next frames out to drop
    // note: this happens at least every time y4m_flush() is called
    // for example when seeking
    if (currDate <= sys->lastDate || sys->lastDate == 0)
    {
        //msg_Err(intf, "currDate <= lastDate");
        //goto ECHO_RETURN;
        sys->lastDate = currDate;
    }

    // reference to first and last picture we are returning
    picture_t* first = NULL;
    picture_t* last = NULL;


    picture_t* pic;
    while( (pic = picture_fifo_Pop(sys->outputFifo)) )
    {
        // do output setup when we see the first frame out from the filter,
        // it could have a different frame rate, chroma, size, etc than
        // the frame going in
        if (!sys->gotFirstOutput)
        {
            sys->gotFirstOutput = true;

            // get the in/out frame ratio by comparing frame rates
            float speed =
                    ((float)srcPic->format.i_frame_rate_base * (float)pic->format.i_frame_rate) /
                    ((float)srcPic->format.i_frame_rate      * (float)pic->format.i_frame_rate_base);

            if (speed < 1.0)
            {
                msg_Err(intf, "frame rate reduction isn't supported yet");
            }
            else
            if (speed > 1.0)
            {
                if (ceil(speed) != speed)
                    msg_Err(intf, "frame rate change must be integer ratio");

                sys->bufferRatio = speed;

                // initial ratio was 1.0, need to correct the number of buffered frames
                // now that we know what it is
                sys->bufferedOut *= sys->bufferRatio;
                sys->minBuffered *= sys->bufferRatio;
            }

            intf->fmt_out.video.i_frame_rate = pic->format.i_frame_rate;
            intf->fmt_out.video.i_frame_rate_base = pic->format.i_frame_rate_base;

            if (intf->fmt_out.video.i_chroma != pic->format.i_chroma)
                msg_Err(intf, "filter changed the chroma, expect corruption");

            // this can't be changed after open, crashes the GLX vout
            //intf->fmt_out.i_codec = pic->format.i_chroma;
            //intf->fmt_out.video.i_chroma = pic->format.i_chroma;

            msg_Info(intf, "first output: buffers=%d:%d", sys->bufferedOut, sys->minBuffered);
        }

        sys->numFrames++;
        sys->bufferedOut--;

        // it seems filter_NewPicture is required now. however,
        // sometimes it returns null in which case it seems like
        // the best thing to do is dump frames
        picture_t* copy = first == NULL ? srcPic : filter_NewPicture(intf);
        if (!copy)
        {
            picture_Release(pic);

            // throw away frames

            // vlc already prints warning for this
            //msg_Err(intf, "filter_NewPicture returns null");
            if (sys->bufferedOut < sys->minBuffered)
                break;
            else
                continue;
        }
        else
        {
            picture_CopyPixels(copy, pic);
            picture_Release(pic);
            pic = copy;
        }

        // the time per output frame interval is a fraction of the input frame time
        int frameTime = (currDate - sys->lastDate) / sys->bufferRatio;

        // the pts is whatever the current pts is minus any buffering
        // introduced by the filter
        pic->date = currDate - sys->bufferedOut*frameTime;

//        msg_Info(intf, "frame=%d buffered=%d:%d frameTime=%d ratio:%d:1 fin=%d:%d fout=%d:%d pts=%u",
//            sys->numFrames, sys->bufferedOut, sys->minBuffered,
//            frameTime, sys->bufferRatio,
//            srcPic->format.i_frame_rate, srcPic->format.i_frame_rate_base,
//            pic->format.i_frame_rate, pic->format.i_frame_rate_base,
//            (unsigned int)pic->date);

        if (last)
            last->p_next = pic;
        else
            first = pic;
        last = pic;


        // if we read too many frames on this iteration, on the next
        // one we might not have any frames available which would be
        // bad as vlc would think our intent was to drop frames
        //
        // if we stop reading before the output is completely empty,
        // there will always be some frames for the next iteration,
        // assuming the filter is fast enough to keep up
        if (sys->bufferedOut  < sys->minBuffered)
            break;

        // if there is still some input buffer left, but the fifo is
        // empty, wait for next frame to arrive. otherwise we can
        // build too much input buffering
        if (sys->bufferedIn > 1)
            waitForOutput(intf);
    }

    if (!first)
    {
        // the buffer checks should prevent from getting here, but
        // just in case prevent leaking the input picture
        picture_Release(srcPic);

        sys->minBuffered++;
    }

    sys->lastDate = currDate;
    return first;
ECHO_RETURN:
    sys->lastDate = currDate;
    //msg_Info(intf, "<<<< filter: ECHO");
    return srcPic;
NULL_RETURN:
    sys->lastDate = currDate;
    picture_Release(srcPic);
    //msg_Info(intf, "<<<< filter: NULL");
    return NULL;
}

/**
 * @brief VLC flush callback
 * @param intf
 */
static void y4m_flush(filter_t* intf)
{
    filter_sys_t* sys = intf->p_sys;

    msg_Info(intf, "flush: enter: buffers=%d:%d:%d",
        sys->bufferedIn, sys->bufferedOut, sys->minBuffered);

    // flush what we can, there can still be frames outstanding
    // after the flush so the counts need to be updated

    // block the input thread while flushing its fifo.
//    vlc_mutex_lock(&sys->inputMutex);
//    picture_t* pic;
//    while ((pic = picture_fifo_Pop(sys->inputFifo)))
//    {
//        picture_Release(pic);
//        sys->bufferedIn--;
//        sys->bufferedOut -= sys->bufferRatio;
//    }
//    vlc_mutex_unlock(&sys->inputMutex);

    picture_t* pic;
    while ((pic = picture_fifo_Pop(sys->outputFifo)))
    {
        picture_Release(pic);
        sys->bufferedOut--;
    }

    // check our accounting, if it goes below zero there is race somewhere
    if (sys->bufferedOut < 0)
    {
        msg_Err(intf, "flush: race condition on output count");
        sys->bufferedOut = 0;
    }

    if (sys->bufferedIn < 0)
    {
        msg_Err(intf, "flush: race condition on input count");
        sys->bufferedIn = 0;
    }

    sys->minBuffered = sys->bufferedOut;
    sys->lastDate = 0;

    msg_Info(intf, "flush: exit:  buffers=%d:%d:%d",
        sys->bufferedIn, sys->bufferedOut, sys->minBuffered);
}

/**
 * @brief VLC module construct callback
 * @return
 */
static int y4m_open(vlc_object_t* obj)
{
    filter_t* intf = (filter_t*)obj;

    // todo: defer this check until we know if its needed or not
    if( !intf->b_allow_fmt_out_change )
    {
        msg_Err(intf, "picture format change isn't allowed");
        return VLC_EGENERIC;
    }

    filter_sys_t* sys = malloc(sizeof(*sys));
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;
    intf->p_sys = sys;
    memset(sys, 0, sizeof(*sys));

    sys->cmd = var_InheritString(intf, "y4m-cmd");
    if (sys->cmd == NULL)
    {
        msg_Err(intf, "argument parse failed");
        free(sys);
        return VLC_EGENERIC;
    }

    sys->echo = var_InheritBool(intf, "y4m-echo");

    sys->stdin = -1;
    sys->stdout = -1;
    sys->bufferRatio = 1;

    msg_Info(intf, "open");

    sys->inputFifo = picture_fifo_New();
    sys->outputFifo = picture_fifo_New();

    vlc_cond_init(&sys->inputCond);
    vlc_cond_init(&sys->outputCond);

    vlc_mutex_init(&sys->inputMutex);
    vlc_mutex_init(&sys->outputMutex);

    intf->pf_video_filter = y4m_filter;
    intf->pf_flush = y4m_flush;

    // todo: the conversion needed isn't known until
    // a frame is read back from the filter, for now
    // filters in/out format needs to be the same
    //intf->fmt_out.video.i_frame_rate *= 2;
    //intf->fmt_out.i_codec = VLC_CODEC_I420;
    //intf->fmt_out.video.i_chroma = VLC_CODEC_I420;

    return VLC_SUCCESS;
}

/**
 * @brief VLC module destruct callback
 * @param obj
 */
static void y4m_close(vlc_object_t* obj)
{
    filter_t* intf = (filter_t*)obj;
    filter_sys_t* sys = intf->p_sys;

    msg_Info(intf, "close");

    // process stop causes thread exit if blocked on write
    stopProcess(intf);

    // cancel causes thread exit if blocked in mutex or cond wait
    if (sys->inputThread)
    {
        vlc_cancel(sys->inputThread);
        vlc_join(sys->inputThread, NULL);
    }

    // output should be dead if input is
    if (sys->outputThread)
        vlc_join(sys->outputThread, NULL);

    picture_fifo_Flush(sys->inputFifo, LAST_MDATE, true);
    picture_fifo_Flush(sys->outputFifo, LAST_MDATE, true);

    picture_fifo_Delete(sys->inputFifo);
    picture_fifo_Delete(sys->outputFifo);

    vlc_cond_destroy(&sys->inputCond);
    vlc_cond_destroy(&sys->outputCond);

    vlc_mutex_destroy(&sys->inputMutex);
    vlc_mutex_destroy(&sys->outputMutex);

    free(sys->cmd);
    free(sys);
}
