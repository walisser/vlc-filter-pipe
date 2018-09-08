import vapoursynth as vs
#import pprint as pp
import functools
import psutil

# get the core instance
core = vs.get_core()

# open a video file; ret is now a clip object
source = core.raws.Source(source='/dev/stdin')

pel_         = 4
blksize_     = 8
search_      = 5
searchparam_ = 32
flow_        = False
enable_      = True

if source.width >= 2560:
    enable_ = False

elif source.width >= 1920:
    pel_     = 1
    blksize_ = 16
    searchparam_ = 16

elif source.width >= 1024:
    pel_         = 2
    blksize_     = 8
    searchparam_ = 12
    flow_        = True

elif source.width >= 640:
    pel_         = 2
    blksize_     = 8
    searchparam_ = 16
    flow_        = True

else:
    pel_         = 4
    blksize_     = 8
    searchparam_ = 16
    flow_        = True

#def animator(n, clip):
#   if n % 2 == 0:
#      return clip
#   else:
#      return core.std.ShufflePlanes(clips=clip, planes=[0, 2, 1], colorfamily=vs.YUV)

avgCpu=0
maxCpu=0
def frameInfo(n, clip):
    global avgCpu, maxCpu
    if n%90 == 0:
        avgCpu = psutil.cpu_percent()
        maxCpu = max(avgCpu, maxCpu)

    return core.text.Text(clip, 'cpu=%d/%d pel=%d blk=%d srchp=%d flow=%d' %
        (avgCpu, maxCpu, pel_, blksize_, searchparam_, flow_), alignment=9 );


if enable_:
    super_ = core.mv.Super(source, pel=pel_, hpad=8, vpad=8)

    back1 = core.mv.Analyse(super_, isb=True, chroma=True, blksize=blksize_,
        search=search_, searchparam=searchparam_)

    fwd1 = core.mv.Analyse(super_, isb=False, chroma=True, blksize=blksize_,
        search=search_, searchparam=searchparam_)

    #back2 = core.mv.Recalculate(super_, back1, chroma=False, blksize=8, blksizev=8, searchparam=0, search=3)

    #fwd2 = core.mv.Recalculate(super_, fwd1, chroma=False, blksize=8, blksizev=8, searchparam=0, search=3)

    # double the fps
    fpsNum = source.fps_num*2
    fpsDen = source.fps_den

    if flow_:
        out = core.mv.FlowFPS(source, super_, back1, fwd1, num=fpsNum, den=fpsDen, mask=2)
    else:
        out = core.mv.BlockFPS(source, super_, back1, fwd1, num=fpsNum, den=fpsDen, mode=3,
            blend=False)

    out = core.std.FrameEval(out, functools.partial(frameInfo, clip=out))

    # set the clip to be output
    out.set_output()

else:
    source.set_output()
