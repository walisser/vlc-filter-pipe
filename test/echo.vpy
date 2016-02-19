import vapoursynth as vs
import pprint as pp

# get the core instance
core = vs.get_core()

# open a video file; ret is now a clip object
source = core.raws.Source(source='/dev/stdin')

# set the clip to be output
source.set_output()
