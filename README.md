wfLZEx
======

Simple compression/decompression program that uses wflz to strip PNG images out of WayForward animation (.anb) archives.
Designed to work with archive files from DuckTales: Remastered and Shantae: Half-Genie Hero, but it may work with other WayForward titles designed around the same timeframe as well.

Internally, the images are stored in a DXT1 or DXT5 format with separate color and multiply channels, described here: http://files.wayforward.com/shane/rgbv/

WayForward's format splits images into pieces and jumbles them up like a jigsaw puzzle so that they'll compress better. This program reconstructs the original images from these pieces and stitches the images together into spritesheets based on animation sequences.

Usage:
wfLZEx.exe [flags] [filenames.anb]

Commandline flags:

--help
Display program usage

--icon
Output a 148x125 TSR-friendly icon along with each sheet

--col-only
Only output color images as described in above link

--mul-only
Only output multiply images as described in above link

--no-sheet
Don't stitch images into sheets. Instead, output frames into subfolders by animation ID

wf3dEx
======

Extract texture, mesh, and bone data from WayForward 3D model (.wf3d) files. Tested with files from Shantae: Half-Genie Hero.

Usage:
wf3dEx.exe [filenames.wf3d]