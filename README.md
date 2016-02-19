# vlc-filter-pipe
Filter plugin for VLC (http://videolan.org) to gain access to external filters via pipe.

# Summary
This plugin adds a new video filter module to VLC (y4mpipe) that can be used to pass frames
to/from external command prior to being displayed by VLC. It opens up the possibilities for
more filtering options such as Vapoursynth scripts and FFmpeg filters. The filter can also
be used in "echo" or one-way mode where it doesn't filter the pictures, only sends them to the
output process.

# Requirements
  * Linux or other UNIX flavor
  * VLC 3.x with chain-static patch
  * Glib 2.x
  
# Compiling
  1. Compile VLC from source with chain-static patch (see below). 
  2. Modify the Makefile PREFIX as needed (defaults /usr/local)
  3. Type make; sudo make install (default /usr/local/vlc/plugins/video_filter/)

# Running

## Test: Copy the input to the output
~~~~bash
vlc --video-filter y4mpipe --y4m-cmd cat file.mp4
~~~~

## FFplay: Mirror VLC output in a separate window (echo mode)
~~~~bash
vlc --video-filter y4mpipe --y4m-cmd "ffplay -autoexit -" --y4m-echo file.mp4
~~~~

## FFmpeg: Swap U/V color planes
~~~~bash
vlc --video-filter y4mpipe --y4m-cmd "ffmpeg -i - -vf swapuv -f yuv4mpegpipe -" file.mp4
~~~~

## Vapoursynth: Motion-Compensated Frame Interpolation (MCFI)
  * Sample script provided in test/mcfi.vpy
  * Requires my fork of vsrawsource for pipe support (https://github.com/walisser/vsrawsource)

~~~~bash
vlc --video-filter y4mpipe --y4m-cmd "vspipe --y4m mcfi.vpy -" file.mp4
~~~~

# Chain-static patch for VLC
This patch inserts the filter into the static filter chain which is currently reserved
for deinterlace and postproc filters. The static chain allows filters to make PTS changes
and supports changing the frame rate.

This is a temporary measure until this function can be added to VLC in a more general manner,
for example via the module descriptor or a command line switch.

~~~~diff
diff --git a/src/video_output/video_output.c b/src/video_output/video_output.c
index cbc8dc0..5b9c46f 100644
--- a/src/video_output/video_output.c
+++ b/src/video_output/video_output.c
@@ -687,7 +687,8 @@ static void ThreadChangeFilters(vout_thread_t *vout,
             e->name = name;
             e->cfg  = cfg;
             if (!strcmp(e->name, "deinterlace") ||
-                !strcmp(e->name, "postproc")) {
+                !strcmp(e->name, "postproc") ||
+                !strcmp(e->name, "y4mpipe")) {
                 vlc_array_append(&array_static, e);
             } else {
                 vlc_array_append(&array_interactive, e);
~~~~
