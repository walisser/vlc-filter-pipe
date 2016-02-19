import vapoursynth as vs
import pprint as pp

# get the core instance
core = vs.get_core()

# open a video file; ret is now a clip object
source = core.raws.Source(source='/dev/stdin')

# convert incompatible chromas
sourceFormat = source.format.id
if sourceFormat==vs.YUV411P8:
    source = core.resize.Bicubic(source, format=vs.YUV420P8)

super_ = core.mv.Super(source)

back1 = core.mv.Analyse(super_, chroma=False, isb=True, blksize=16, searchparam=3, plevel=0, search=5)

fwd1 = core.mv.Analyse(super_, chroma=False, isb=False, blksize=16, searchparam=3, plevel=0, search=5)

#back2 = core.mv.Recalculate(super_, back1, chroma=False, blksize=8, blksizev=8, searchparam=0, search=5)

#fwd2 = core.mv.Recalculate(super_, fwd1, chroma=False, blksize=8, blksizev=8, searchparam=0, search=5)

# double the fps
fpsNum = source.fps_num*2
fpsDen = source.fps_den

out = core.mv.BlockFPS(source, super_, back1, fwd1, num=fpsNum, den=fpsDen, mode=3, thscd2=12)

# put back in original format
if sourceFormat != out.format.id:
    out = core.resize.Bicubic(out, format=sourceFormat)

# set the clip to be output
out.set_output()
