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
using namespace std;

int g_DecompressFlags;
bool g_bSeparate;
bool g_bPieceTogether;
bool g_bColOnly;
bool g_bMulOnly;

typedef struct
{
	uint32_t headerSz;	//Probably
	uint32_t unk0;		//Some kind of number of something?
	uint32_t numFrames;	//Number of frames in the animation
	uint32_t numFrames2;//Number of something else? (numFrames2 + numFrames = total number of framePtrs)
	uint32_t unk2;		// 0x100 is what?
	uint32_t unk3;		//0x77F is what?
	uint32_t ptrOffset;	//point to framePtr[]
} anbHeader;

typedef struct
{
	uint64_t frameOffset;	//point to FrameDesc
} framePtr;		//Repeat anbHeader.numFrames + anbHeader.numFrames2 times

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
	uint64_t texOffset; // point to texOffset
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
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint8_t a;
} pixel;

#define PALETTE_SIZE					256

#define TEXTURE_TYPE_256_COL			1	//256-color 4bpp palette followed by pixel data
#define TEXTURE_TYPE_DXT1_COL			2	//squish::kDxt1 color, no multiply
#define TEXTURE_TYPE_DXT5_COL			3	//squish::kDxt5 color, no multiply
#define TEXTURE_TYPE_DXT1_COL_MUL		5	//squish::kDxt1 color and squish::kDxt1 multiply
#define TEXTURE_TYPE_DXT5_COL_DXT1_MUL	6	//squish::kDxt5 color and squish::kDxt1 multiply



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
	size_t amt = fread(&fileData, fileSize, 1, fh);
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
	
	//Parse through, splitting out before each ZLFW header
	int iCurFile = 0;
	uint64_t startPos = 0;
	for(uint64_t i = 0; i < fileSize; i++)	//Definitely not the fastest way to do it... but I don't care
	{
		if(memcmp ( &(fileData[i]), "ZLFW", 4 ) == 0)	//Found another file
		{
			bool bPieceTogether = g_bPieceTogether;
			//Get texture header
			uint64_t headerPos = i - sizeof(texHeader);
			texHeader th;
			memcpy(&th, &(fileData[headerPos]), sizeof(texHeader));
				
			//Search for frame header if we're going to be piecing these together
			FrameDesc fd;
			if(bPieceTogether)
			{
				if(iCurFile == 0)	//First one tells us where to start searching backwards from
					startPos = i;
				for(int k = startPos - sizeof(texHeader) - sizeof(FrameDesc); k > 0; k -= 4)
				{
					memcpy(&fd, &(fileData[k]), sizeof(FrameDesc));
					if(fd.texOffset != headerPos) continue;
					//Sanity check
					if(fd.texDataSize > 6475888) continue;	//TODO: Test to make sure data size matches
					if(fd.texOffset == 0 || fd.pieceOffset == 0) continue;
					if(fd.pieceOffset > fileSize) continue;
					
					//Ok, found our header
					break;
				}
			}
			
			//Decompress WFLZ data
			uint32_t* chunk = NULL;
			const uint32_t decompressedSize = wfLZ_GetDecompressedSize( &(fileData[i]) );
			uint8_t* dst = ( uint8_t* )malloc( decompressedSize );
			uint32_t offset = 0;
			int count = 0;
			while( uint8_t* compressedBlock = wfLZ_ChunkDecompressLoop( &(fileData)[i], &chunk ) )
			{		
				wfLZ_Decompress( compressedBlock, dst + offset );
				const uint32_t blockSize = wfLZ_GetDecompressedSize( compressedBlock );
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
					squish::DecompressImage( color, th.width, th.height, dst, squish::kDxt1 );
				
				//Create multiply image
				mul = (uint8_t*)malloc(decompressedSize * 8);
				if(!g_bColOnly)
					squish::DecompressImage( mul, th.width, th.height, dst + decompressedSize/2, squish::kDxt1 );	//Second image starts halfway through decompressed data
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
			
			//Read in pieces
			Vec2 maxul;
			Vec2 maxbr;
			maxul.x = maxul.y = maxbr.x = maxbr.y = 0;
			list<piece> pieces;
			
			if(bPieceTogether)
			{
				PiecesDesc pd;
				memcpy(&pd, &(fileData[fd.pieceOffset]), sizeof(PiecesDesc));
				for(uint32_t j = 0; j < pd.numPieces; j++)
				{
					piece p;
					memcpy(&p, &(fileData[fd.pieceOffset+j*sizeof(piece)+sizeof(PiecesDesc)]), sizeof(piece));
					//Store our maximum values, so we know how large the image is
					if(p.topLeft.x < maxul.x)
						maxul.x = p.topLeft.x;
					if(p.topLeft.y > maxul.y)
						maxul.y = p.topLeft.y;
					if(p.bottomRight.x > maxbr.x)
						maxbr.x = p.bottomRight.x;
					if(p.bottomRight.y < maxbr.y)
						maxbr.y = p.bottomRight.y;
					
					//cout << p.topLeft.x << ", " << p.topLeft.y << ", " << p.topLeftUV.x  << ", " << p.topLeftUV.y << ", " << p.bottomRight.x << ", " << p.bottomRight.y << ", " << p.bottomRightUV.x << ", " << p.bottomRightUV.y << endl;
					
					pieces.push_back(p);
				}
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
			
			FIBITMAP* result = NULL;
			if(bPieceTogether && pieces.size())
				result = PieceImage(dest_final, pieces, maxul, maxbr, th);
			else
				result = imageFromPixels(dest_final, th.width, th.height);
			
			ostringstream oss;
			oss << "output/" << sName << "_" << iCurFile+1 << ".png";
			cout << "Saving " << oss.str() << endl;
			FreeImage_Save(FIF_PNG, result, oss.str().c_str());
			FreeImage_Unload(result);
			
			//Free allocated memory
			free(dst);
			free(dest_final);
			
			iCurFile++;
		}
	}
	free(fileData);
	return 0;
}

int main(int argc, char** argv)
{
	g_DecompressFlags = squish::kDxt1;
	g_bSeparate = false;
	g_bPieceTogether = true;
	g_bColOnly = g_bMulOnly = false;
	FreeImage_Initialise();
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
		else
			sFilenames.push_back(s);
	}
	//Decompress ANB files
	for(list<string>::iterator i = sFilenames.begin(); i != sFilenames.end(); i++)
		splitImages((*i).c_str());
	FreeImage_DeInitialise();
	return 0;
}
