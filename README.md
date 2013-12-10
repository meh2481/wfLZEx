wfLZEx
======

Simple compression/decompression program for a friend that uses wflz to strip PNG images out of .anb archives.
Designed to work with archive files from DuckTales: Remastered, but for all I know it'll work with other WayForward games as well.

Internally, the images are stored in a fancy proprietary DXT-1 format, described here: http://files.wayforward.com/shane/rgbv/

WayForward's format splits images into pieces and jumbles them up like a jigsaw puzzle so that they'll compress better. This program attempts to reconstruct the original images from these pieces. There are some rounding errors that get introduced in the process, so the output images may not be 100% correct (Though from what I've tested, the artifacts aren't noticable unless you reconstruct color/multiply images seperately). If perfect accuracy is important to you, you may want to pass along the commandline flag "-nopiece" and reconstruct the images by hand.

Usage:
wfLZEx.exe [flags] [filenames.anb]

Commandline flags:

-dxt1
Treat images as if they're DXT1-compressed (default)

-dxt3
Treat images as if they're DXT3-compressed (probably won't produce valid output)

-dxt5
Treat images as if they're DXT5-compressed (probably won't produce valid output)

-separate
Output separate images for color and multiply (default is off)

-col-only
Same as above, but only output color images

-mul-only
same as -separate, but only output multiply images

-nopiece
Don't attempt to reconstruct images from image piece data (default: attempt to reconstruct images)

