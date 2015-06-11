#include "wfLZ.h"
#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <stdlib.h>
#include <cstdio>
#include <squish.h>
#ifdef _WIN32
	#include <windows.h>
#endif
#include "FreeImage.h"
#include <list>
#include <cmath>
#include <cstring>
#include <iomanip>
using namespace std;

int g_DecompressFlags;
bool g_bSeparate;
bool g_bPieceTogether;
bool g_bColOnly;
bool g_bMulOnly;
bool g_bCrop;

int offsetX = 1;
int offsetY = 2;


//------------------------------
// ANB file format structs
//------------------------------
typedef struct
{
	uint32_t headerSz;	//Probably
	uint32_t unk0;		//Some kind of number of something?
	uint32_t numFrames;	//Number of animation frames in the file
	uint32_t numAnimations;
	uint32_t unk2;		// 0x100 is what?
	uint32_t unk3;		//0x77F is what?
	uint64_t ptrOffset;	//point to frame framePtr[]
	uint64_t animOffset;//point to anim framePtr[]
} anbHeader;

typedef struct
{
	uint64_t frameOffset;	//point to FrameDesc or animHeader
} framePtr;		//Repeat anbHeader.numFrames + anbHeader.numAnimations times

typedef struct
{
	uint32_t type;
	uint32_t width;
	uint32_t height;
	uint32_t unknown1[5];
	//uint8_t data[]		//Followed by image data
} texHeader;

typedef struct  
{
	int      unknown0[4];
	uint64_t texOffset; // point to texHeader
	int      texDataSize;
	int      unknown1;
	uint64_t pieceOffset; // point to PiecesDesc
} FrameDesc;

typedef struct
{
	float x;
	float y;
} Vec2;

typedef struct 
{
	Vec2 topLeft;
	Vec2 topLeftUV;
	Vec2 bottomRight;
	Vec2 bottomRightUV;
} piece;

typedef struct
{
	uint32_t numPieces;
	//piece[]				//Followed by numPieces pieces
} PiecesDesc;

typedef struct
{
	uint32_t animIDHash;	//We don't care
	uint32_t numFrames;		//How many frames in the animation
	uint32_t unk0[2];
	uint64_t animListPtr;	//point to animFrameList[]
	uint32_t unk1[2];
}animHeader;

typedef struct
{
	uint64_t offset;	//point to animFrame
}animFrameList;	//Repeat for animHeader.numFrames

typedef struct
{
	uint32_t frameNo;
	int unk0[10];
	//uint32_t unk1;
	//float unk2[8];
}animFrame;


//------------------------------
// Helper structs
//------------------------------
typedef struct
{
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint8_t a;
} pixel;

typedef struct
{
	Vec2 maxul;
	Vec2 maxbr;
	FIBITMAP* img;
} frameImg;

typedef struct
{
	Vec2 maxul;
	Vec2 maxbr;
	uint32_t animIDHash;
	list<uint32_t> animFrames;
} animHelper;

typedef struct
{
	Vec2 maxul;
	Vec2 maxbr;
	texHeader th;
	uint8_t* data;
} frameSizeHelper;


#define PALETTE_SIZE					256

#define TEXTURE_TYPE_256_COL			1	//256-color 4bpp palette followed by pixel data
#define TEXTURE_TYPE_DXT1_COL			2	//squish::kDxt1 color, no multiply
#define TEXTURE_TYPE_DXT5_COL			3	//squish::kDxt5 color, no multiply
#define TEXTURE_TYPE_DXT1_COL_MUL		5	//squish::kDxt1 color and squish::kDxt1 multiply
#define TEXTURE_TYPE_DXT5_COL_DXT1_MUL	6	//squish::kDxt5 color and squish::kDxt1 multiply

//Crop all alpha=0 space around an image input, update maxul/br accordingly
frameImg cropImage(frameImg input)
{
	//Grab image bits
	int width = FreeImage_GetWidth(input.img);
	int height = FreeImage_GetHeight(input.img);
	unsigned pitch = FreeImage_GetPitch(input.img);
	BYTE* bits = (BYTE*)FreeImage_GetBits(input.img);
	
	//Keep track of extents
	int cropTop = height;
	int cropLeft = width;
	int cropRight = 0;
	int cropBottom = 0;
	
	//Spin through image
	for(int y = 0; y < height; y++)
	{
		BYTE* pixel = bits;
		for(int x = 0; x < width; x++)
		{
			if(pixel[FI_RGBA_ALPHA])	//Pixel with nonzero alpha value
			{
				if(y < cropTop)
					cropTop = y;
				if(x < cropLeft)
					cropLeft = x;
				if(y > cropBottom)
					cropBottom = y;
				if(x > cropRight)
					cropRight = x;
			}
			pixel += 4;
		}
		bits += pitch;
	}
	
	if(cropLeft || cropTop || cropRight+1 < width || cropBottom+1 < height)
	{
		RGBQUAD q = {255,0,0,255};
		FIBITMAP* result = FreeImage_EnlargeCanvas(input.img, -cropLeft, -(height-(cropBottom+1)), -(width-(cropRight+1)), -cropTop, &q, FI_COLOR_IS_RGB_COLOR);
		FreeImage_Unload(input.img);
		input.img = result;
		
		input.maxul.x += cropLeft;
		input.maxul.y -= cropTop;
		input.maxbr.x -= cropRight;
		input.maxbr.y += cropBottom;
	}
	
	return input;
}

void multiply(uint8_t* dest_final, uint8_t* color, uint8_t* mul, texHeader th, bool bUseMulAlpha)
{
	//Multiply two images together
	for(int j = 0; j < th.width * th.height * 4; j += 4)
	{
		if(color[j+3] != 255)
		{
			dest_final[j] = dest_final[j+1] = dest_final[j+2] = 0;
			dest_final[j+3] = mul[j+1];
		}
		else
		{
			uint8_t mask = 255 - mul[j+1];
			float fMul = (float)(mask)/255.0;
			dest_final[j] = color[j]*fMul;
			dest_final[j+1] = color[j+1]*fMul;
			dest_final[j+2] = color[j+2]*fMul;
			if(!bUseMulAlpha)
				dest_final[j+3] = 255;
			else
				dest_final[j+3] = mul[j+3];
		}				
	}
}

FIBITMAP* imageFromPixels(uint8_t* imgData, uint32_t width, uint32_t height)
{
	//FreeImage is broken here and you can't swap R/G/B channels upon creation. Do that manually
	FIBITMAP* result = FreeImage_ConvertFromRawBits(imgData, width, height, ((((32 * width) + 31) / 32) * 4), 32, FI_RGBA_RED, FI_RGBA_GREEN, FI_RGBA_BLUE, true);
	FIBITMAP* r = FreeImage_GetChannel(result, FICC_RED);
	FIBITMAP* b = FreeImage_GetChannel(result, FICC_BLUE);
	FreeImage_SetChannel(result, b, FICC_RED);
	FreeImage_SetChannel(result, r, FICC_BLUE);
	FreeImage_Unload(r);
	FreeImage_Unload(b);
	return result;
}

FIBITMAP* PieceImage(uint8_t* imgData, list<piece> pieces, Vec2 maxul, Vec2 maxbr, texHeader th, bool bFillBlack = false, bool bAdd = true);

FIBITMAP* PieceImage(uint8_t* imgData, list<piece> pieces, Vec2 maxul, Vec2 maxbr, texHeader th, bool bFillBlack, bool bAdd)
{
	Vec2 OutputSize;
	Vec2 CenterPos;
	OutputSize.x = -maxul.x + maxbr.x;
	OutputSize.y = maxul.y - maxbr.y;
	CenterPos.x = -maxul.x;
	CenterPos.y = maxul.y;
	OutputSize.x = (uint32_t)(OutputSize.x + 0.5f);
	OutputSize.y = (uint32_t)(OutputSize.y + 0.5f);

	FIBITMAP* result = FreeImage_Allocate(OutputSize.x, OutputSize.y, 32);
	
	//Fill this image black (Important for multiply images)
	if(bFillBlack)
	{
		RGBQUAD q = {0,0,0,255};
		FreeImage_FillBackground(result, &q);
	}

	//Create image from this set of pixels
	FIBITMAP* curImg = imageFromPixels(imgData, th.width, th.height);

	//Patch image together from pieces
	for(list<piece>::iterator lpi = pieces.begin(); lpi != pieces.end(); lpi++)
	{
		FIBITMAP* imgPiece = FreeImage_Copy(curImg, 
											(int)((lpi->topLeftUV.x) * th.width + 0.5f), (int)((lpi->topLeftUV.y) * th.height + 0.5f), 
											(int)((lpi->bottomRightUV.x) * th.width + 0.5f), (int)((lpi->bottomRightUV.y) * th.height + 0.5f));
		
		//Paste this into the pieced image
		Vec2 DestPos = CenterPos;
		DestPos.x += lpi->topLeft.x;
		DestPos.y -= lpi->topLeft.y;	//y is negative here
		DestPos.x = (uint32_t)(DestPos.x + 0.5f);
		DestPos.y = (uint32_t)(DestPos.y + 0.5f);
		
		FreeImage_Paste(result, imgPiece, DestPos.x, DestPos.y, 256);
		FreeImage_Unload(imgPiece);
	}
	FreeImage_Unload(curImg);
	
	return result;
}


int splitImages(const char* cFilename)
{
	bool bPieceTogether = g_bPieceTogether;
	uint8_t* fileData;
	FILE* fh = fopen(cFilename, "rb");
	if(fh == NULL)
	{
		cerr << "Unable to open input file " << cFilename << endl;
		return 1;
	}
	fseek(fh, 0, SEEK_END);
	size_t fileSize = ftell(fh);
	fseek(fh, 0, SEEK_SET);
	fileData = (uint8_t*)malloc(fileSize);
	size_t amt = fread(fileData, fileSize, 1, fh);
	fclose(fh);
	cout << "Splitting images from file " << cFilename << endl;
	
	//Figure out what we'll be naming the images
	string sName = cFilename;
	//First off, strip off filename extension
	size_t namepos = sName.find(".anb");
	if(namepos != string::npos)
		sName.erase(namepos);
	//Next, strip off any file path before it
	namepos = sName.rfind('/');
	if(namepos == string::npos)
		namepos = sName.rfind('\\');
	if(namepos != string::npos)
		sName.erase(0, namepos+1);
		
	//Grab ANB Header
	anbHeader ah;
	memcpy(&ah, fileData, sizeof(anbHeader));
	
	//First, parse through and get pieces, so we know maxul/br for each frame
	vector<list<piece> > framePieces;	//Keep track of the pieces for each frame
	vector<frameSizeHelper> frameSizes;	//Keep track of the maximum sizes for each frame (and the image data itself)
	if(bPieceTogether)
	{
		for(uint64_t i = 0; i < ah.numFrames; i++)
		{
			//Get frame pointer
			framePtr fp;
			memcpy(&fp, &fileData[ah.ptrOffset + (i * sizeof(framePtr))], sizeof(framePtr));
					
			//Grab framedesc header
			FrameDesc fd;
			memcpy(&fd, &fileData[fp.frameOffset], sizeof(FrameDesc));
			
			//Read in pieces
			frameSizeHelper fsh;
			fsh.data = NULL;
			fsh.maxul.x = fsh.maxul.y = fsh.maxbr.x = fsh.maxbr.y = 0;
			list<piece> pieces;
			
			PiecesDesc pd;
			memcpy(&pd, &(fileData[fd.pieceOffset]), sizeof(PiecesDesc));
			for(uint32_t j = 0; j < pd.numPieces; j++)
			{
				piece p;
				memcpy(&p, &(fileData[fd.pieceOffset+j*sizeof(piece)+sizeof(PiecesDesc)]), sizeof(piece));
				//Store our maximum values, so we know how large the image is
				if(p.topLeft.x < fsh.maxul.x)
					fsh.maxul.x = p.topLeft.x;
				if(p.topLeft.y > fsh.maxul.y)
					fsh.maxul.y = p.topLeft.y;
				if(p.bottomRight.x > fsh.maxbr.x)
					fsh.maxbr.x = p.bottomRight.x;
				if(p.bottomRight.y < fsh.maxbr.y)
					fsh.maxbr.y = p.bottomRight.y;
				
				//if(i >= 151 && i <= 154)
				//	cout << p.topLeft.x << ", " << p.topLeft.y << ", " << p.topLeftUV.x  << ", " << p.topLeftUV.y << ", " << p.bottomRight.x << ", " << p.bottomRight.y << ", " << p.bottomRightUV.x << ", " << p.bottomRightUV.y << endl;
				
				pieces.push_back(p);
			}
			frameSizes.push_back(fsh);
			framePieces.push_back(pieces);
		}
	}
	
	//Parse through, splitting each image out
	for(uint64_t i = 0; i < ah.numFrames; i++)
	{
		//Get frame pointer
		framePtr fp;
		memcpy(&fp, &fileData[ah.ptrOffset + (i * sizeof(framePtr))], sizeof(framePtr));
				
		//Grab framedesc header
		FrameDesc fd;
		memcpy(&fd, &fileData[fp.frameOffset], sizeof(FrameDesc));
		
		//Get texture header
		texHeader th;
		memcpy(&th, &fileData[fd.texOffset], sizeof(texHeader));
		
		uint64_t dataOffset = fd.texOffset + sizeof(texHeader);
		//Decompress WFLZ data
		uint32_t* chunk = NULL;
		const uint32_t decompressedSize = wfLZ_GetDecompressedSize(&(fileData[dataOffset]));
		uint8_t* dst = (uint8_t*)malloc(decompressedSize);
		uint32_t offset = 0;
		int count = 0;
		while(uint8_t* compressedBlock = wfLZ_ChunkDecompressLoop(&(fileData)[dataOffset], &chunk))
		{		
			wfLZ_Decompress(compressedBlock, dst + offset);
			const uint32_t blockSize = wfLZ_GetDecompressedSize(compressedBlock);
			offset += blockSize;
		}
		
		//Decompress image
		uint8_t* color = NULL;
		uint8_t* mul = NULL;
		bool bUseMul = false;
		if(th.type == TEXTURE_TYPE_DXT1_COL_MUL)
		{
			//Create color image
			color = (uint8_t*)malloc(decompressedSize * 8);
			if(!g_bMulOnly)
				squish::DecompressImage(color, th.width, th.height, dst, squish::kDxt1);
			
			//Create multiply image
			mul = (uint8_t*)malloc(decompressedSize * 8);
			if(!g_bColOnly)
				squish::DecompressImage(mul, th.width, th.height, dst + decompressedSize/2, squish::kDxt1);	//Second image starts halfway through decompressed data
		}
		else if(th.type == TEXTURE_TYPE_DXT1_COL)
		{
			color = (uint8_t*)malloc(decompressedSize * 8);
			squish::DecompressImage(color, th.width, th.height, dst, squish::kDxt1);
		}
		else if(th.type == TEXTURE_TYPE_DXT5_COL)
		{
			color = (uint8_t*)malloc(th.width * th.height * 4);
			squish::DecompressImage(color, th.width, th.height, dst, squish::kDxt5);
		}
		else if(th.type == TEXTURE_TYPE_256_COL)
		{
			//Read in palette
			vector<pixel> palette;
			uint8_t* cur_data_ptr = dst;
			for(uint32_t curPixel = 0; curPixel < PALETTE_SIZE; curPixel++)
			{
				pixel p;
				p.r = *cur_data_ptr++;
				p.g = *cur_data_ptr++;
				p.b = *cur_data_ptr++;
				p.a = *cur_data_ptr++;
				palette.push_back(p);
			}
			
			//Fill in image
			color = (uint8_t*)malloc(th.width * th.height * 4);
			uint8_t* cur_color_ptr = color;
			for(uint32_t curPixel = 0; curPixel < th.width * th.height; curPixel++)
			{
				*cur_color_ptr++ = palette[*cur_data_ptr].b;
				*cur_color_ptr++ = palette[*cur_data_ptr].g;
				*cur_color_ptr++ = palette[*cur_data_ptr].r;
				*cur_color_ptr++ = palette[*cur_data_ptr].a;
				cur_data_ptr++;
			}
		}
		else if(th.type == TEXTURE_TYPE_DXT5_COL_DXT1_MUL)
		{
			color = (uint8_t*)malloc(th.width * th.height * 4);
			squish::DecompressImage(color, th.width, th.height, dst, squish::kDxt1);
			
			mul = (uint8_t*)malloc(th.width * th.height * 4);
			squish::DecompressImage(mul, th.width, th.height, dst + th.width * th.height / 2, squish::kDxt5);
			
			bUseMul = true;
		}
		else
		{
			cout << "Decomp size: " << decompressedSize << ", w*h: " << th.width << "," << th.height << endl;
			cout << "Warning: skipping unknown image type " << th.type << endl;
			delete dst;
			continue;
		}
		
		//Multiply images together if need be
		uint8_t* dest_final = NULL;
		if(color != NULL && mul != NULL)
		{
			dest_final = (uint8_t*)malloc(decompressedSize * 8);
			multiply(dest_final, color, mul, th, bUseMul);
			free(color);
			free(mul);
		}
		else if(color != NULL)
			dest_final = color;
		else if(mul != NULL)
			dest_final = mul;
		else	//Should be unreachable
		{
			cout << "ERR: Unreachable" << endl;
			free(dst);
			continue;
		}
		
		//Don't piece now; save data for piecing later
		frameSizes[i].data = dest_final;
		frameSizes[i].th = th;
		
		//Free allocated memory
		free(dst);
	}
	
	//Grab animation frames
	vector<animHelper> animations;
	for(int i = 0; i < ah.numAnimations; i++)
	{
		//Get pointer to anim header
		framePtr fp;
		memcpy(&fp, &fileData[ah.animOffset + (i*sizeof(framePtr))], sizeof(framePtr));
		
		//Get anim header
		animHeader anh;
		memcpy(&anh, &fileData[fp.frameOffset], sizeof(animHeader));
		
		animHelper anmh;
		anmh.animIDHash = anh.animIDHash;
		anmh.maxul.x = anmh.maxul.y = anmh.maxbr.x = anmh.maxbr.y = 0;
		for(int j = 0; j < anh.numFrames; j++)
		{
			animFrameList afl;
			memcpy(&afl, &fileData[anh.animListPtr + (j*sizeof(animFrameList))], sizeof(animFrameList));
			
			animFrame anf;
			memcpy(&anf, &fileData[afl.offset], sizeof(animFrame));
			
			anmh.animFrames.push_back(anf.frameNo);
			
			//Store maximum extents for this animation
			if(frameSizes[anf.frameNo].maxul.x < anmh.maxul.x)
				anmh.maxul.x = frameSizes[anf.frameNo].maxul.x;
			if(frameSizes[anf.frameNo].maxul.y > anmh.maxul.y)
				anmh.maxul.y = frameSizes[anf.frameNo].maxul.y;
			if(frameSizes[anf.frameNo].maxbr.x > anmh.maxbr.x)
				anmh.maxbr.x = frameSizes[anf.frameNo].maxbr.x;
			if(frameSizes[anf.frameNo].maxbr.y < anmh.maxbr.y)
				anmh.maxbr.y = frameSizes[anf.frameNo].maxbr.y;
		}
		animations.push_back(anmh);
	}
	
	//Figure out dimensions of final image
	int finalX = offsetX;
	int finalY = offsetY/2;
	int totalWidthAvg = 0;
	//First loop: Figure out maximum width of each animation
	for(vector<animHelper>::iterator i = animations.begin(); i != animations.end(); i++)
	{
		int animMaxX = (int)(i->maxbr.x - i->maxul.x + 0.5);
		int animMaxY = (int)(i->maxul.y - i->maxbr.y + 0.5);
		
		animMaxX += offsetX;
		animMaxX *= i->animFrames.size();
		
		if(finalX < animMaxX)
			finalX = animMaxX + offsetX;
		totalWidthAvg += animMaxX + offsetX;
		
		finalY += offsetY + animMaxY;
	}
	
	//Don't care if there's only a few
	if(animations.size() > 5)
		totalWidthAvg /= animations.size();
	totalWidthAvg *= 2;
	
	//If an animation is longer than double the size of the average, cut it up vertically
	finalX = offsetX;
	finalY = offsetY/2;
	for(vector<animHelper>::iterator i = animations.begin(); i != animations.end(); i++)
	{
		int animMaxX = (int)(i->maxbr.x - i->maxul.x + 0.5);
		int animMaxY = (int)(i->maxul.y - i->maxbr.y + 0.5);
		
		animMaxX += offsetX;
		int curAnimMaxX = 0;
		for(int j = 0; j < i->animFrames.size(); j++)
		{
			if(curAnimMaxX + animMaxX > totalWidthAvg)
			{
				if(finalX < curAnimMaxX)				//Save final width
					finalX = curAnimMaxX + offsetX;
				
				finalY += offsetY/2 + animMaxY;			//Offset vertically, spacing of 1 pixel instead of 2
				curAnimMaxX = 0;						//Start next row
			}
			curAnimMaxX += animMaxX;
		}
		
		if(finalX < curAnimMaxX)
			finalX = curAnimMaxX + offsetX;
		
		finalY += offsetY + animMaxY;
	}
	
	//Allocate final image, and piece
	FIBITMAP* finalSheet = FreeImage_Allocate(finalX, finalY, 32);
	RGBQUAD q = {128,128,0,255};
	FreeImage_FillBackground(finalSheet, (const void *)&q);
	
	int curX = offsetX;
	int curY = offsetY/2;
	//Now loop through, building images and stitching frames into the final image
	for(vector<animHelper>::iterator i = animations.begin(); i != animations.end(); i++)
	{
		uint32_t curFrame = 1;
		int yAdd = 0;
		for(list<uint32_t>::iterator j = i->animFrames.begin(); j != i->animFrames.end(); j++)
		{
			//Now that we have the maximum extents for each animation, we can build the frames
			FIBITMAP* result = NULL;
			if(bPieceTogether && framePieces[*j].size())
				result = PieceImage(frameSizes[*j].data, framePieces[*j], i->maxul, i->maxbr, frameSizes[*j].th);
			else
				result = imageFromPixels(frameSizes[*j].data, frameSizes[*j].th.width, frameSizes[*j].th.height);
				
			yAdd = offsetY + FreeImage_GetHeight(result);
			
			//See if we should start next row in sprite sheet (if this anim is too long)
			if(curX + FreeImage_GetWidth(result) + offsetX > totalWidthAvg)
			{
				curX = offsetX;
				curY += FreeImage_GetHeight(result) + offsetY/2;
			}
			
			//Paste this into our final image
			FreeImage_Paste(finalSheet, result, curX, curY, 300);
			
			//ostringstream oss;
			//oss << "output/" << sName << '_' << i->animIDHash << '_' << setw(3) << setfill('0') << *j << ".png";
			//cout << "Saving " << oss.str() << endl;
			//FreeImage_Save(FIF_PNG, result, oss.str().c_str());
		
			curX += offsetX + FreeImage_GetWidth(result);
			FreeImage_Unload(result);
		}
		curY += yAdd;
		curX = offsetX;
	}
	
	//Save final sheet
	ostringstream oss;
	oss << "output/" << sName << ".png";
	cout << "Saving " << oss.str() << endl;
	FreeImage_Save(FIF_PNG, finalSheet, oss.str().c_str());
	FreeImage_Unload(finalSheet);
	
	for(vector<frameSizeHelper>::iterator i = frameSizes.begin(); i != frameSizes.end(); i++)
		free(i->data);
	frameSizes.clear();
	
	free(fileData);
	return 0;
}

int main(int argc, char** argv)
{
	g_DecompressFlags = squish::kDxt1;
	g_bSeparate = false;
	g_bPieceTogether = true;
	g_bColOnly = g_bMulOnly = false;
	g_bCrop = false;
	FreeImage_Initialise();
	//Create output folder	
#ifdef _WIN32
	CreateDirectory(TEXT("output"), NULL);
#else
	int result = system("mkdir -p output");
#endif
	list<string> sFilenames;
	//Parse commandline
	for(int i = 1; i < argc; i++)
	{
		string s = argv[i];
		if(s == "-dxt1")
			g_DecompressFlags = squish::kDxt1;
		else if(s == "-dxt3")
			g_DecompressFlags = squish::kDxt3;
		else if(s == "-dxt5")
			g_DecompressFlags = squish::kDxt5;
		else if(s == "-separate")
			g_bSeparate = true;
		else if(s == "-col-only")
		{
			g_bColOnly = true;
			g_bMulOnly = false;
			g_bSeparate = true;
		}
		else if(s == "-mul-only")
		{
			g_bMulOnly = true;
			g_bColOnly = false;
			g_bSeparate = true;
		}
		else if(s == "-nopiece")
			g_bPieceTogether = false;
		else if(s == "-crop")
			g_bCrop = true;
		else
			sFilenames.push_back(s);
	}
	//Decompress ANB files
	for(list<string>::iterator i = sFilenames.begin(); i != sFilenames.end(); i++)
		splitImages((*i).c_str());
	FreeImage_DeInitialise();
	return 0;
}
