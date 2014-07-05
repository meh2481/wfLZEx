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
	uint32_t type;
	uint32_t width;
	uint32_t height;
	uint32_t unknown1[5];
	//uint8_t data[]
} texHeader;

typedef struct  
{
	int      unknown0[4];
	uint64_t texOffset; // offset from start of file to TexDesc
	int      texDataSize;
	int      unknown1;
	uint64_t pieceOffset; // offset from start of file to PiecesDesc
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
	//piece[]
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

int powerof2(int orig)
{
	int result = 1;
	while(result < orig)
		result <<= 1;
	return result;
}

void multiply(uint8_t* dest_final, uint8_t* color, uint8_t* mul, texHeader th)
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
			dest_final[j+3] = 255;
		}				
	}
}

FIBITMAP* imageFromPixels(uint8_t* imgData, uint32_t width, uint32_t height)
{
	//return FreeImage_ConvertFromRawBits(imgData, width, height, width*4, 32, 0xFF0000, 0x00FF00, 0x0000FF, true);	//Doesn't seem to work
	FIBITMAP* curImg = FreeImage_Allocate(width, height, 32);
	FREE_IMAGE_TYPE image_type = FreeImage_GetImageType(curImg);
	if(image_type == FIT_BITMAP)
	{
		int curPos = 0;
		unsigned pitch = FreeImage_GetPitch(curImg);
		BYTE* bits = (BYTE*)FreeImage_GetBits(curImg);
		bits += pitch * height - pitch;
		for(int y = height-1; y >= 0; y--)
		{
			BYTE* pixel = (BYTE*)bits;
			for(int x = 0; x < width; x++)
			{
				pixel[FI_RGBA_RED] = imgData[curPos++];
				pixel[FI_RGBA_GREEN] = imgData[curPos++];
				pixel[FI_RGBA_BLUE] = imgData[curPos++];
				pixel[FI_RGBA_ALPHA] = imgData[curPos++];
				pixel += 4;
			}
			bits -= pitch;
		}
	}
	return curImg;
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
	OutputSize.x = uint32_t(OutputSize.x);
	OutputSize.y = uint32_t(OutputSize.y);

	FIBITMAP* result = FreeImage_Allocate(OutputSize.x+6, OutputSize.y+6, 32);
	
	//Fill this image black (Important for multiply images)
	if(bFillBlack)
	{
		int width = FreeImage_GetWidth(result);
		int height = FreeImage_GetHeight(result);
		unsigned pitch = FreeImage_GetPitch(result);
		BYTE* bits = (BYTE*)FreeImage_GetBits(result);
		for(int y = 0; y < height; y++)
		{
			BYTE* pixel = bits;
			for(int x = 0; x < width; x++)
			{
				pixel[FI_RGBA_RED] = pixel[FI_RGBA_GREEN] = pixel[FI_RGBA_BLUE] = 0;
				pixel[FI_RGBA_ALPHA] = 255;
				pixel += 4;
			}
			bits += pitch;
		}
	}

	//Create image from this set of pixels
	FIBITMAP* curImg = imageFromPixels(imgData, th.width, th.height);

	//Patch image together from pieces
	for(list<piece>::iterator lpi = pieces.begin(); lpi != pieces.end(); lpi++)
	{
		float add = 0;
		if(bAdd)
			add = 0.001;
		FIBITMAP* imgPiece = FreeImage_Copy(curImg, 
											floor((lpi->topLeftUV.x) * th.width - add), floor((lpi->topLeftUV.y) * th.height - add), 
											ceil((lpi->bottomRightUV.x) * th.width + add), ceil((lpi->bottomRightUV.y) * th.height + add));
		
		//Since pasting doesn't allow you to post an image onto a particular position of another, do that by hand
		int curPos = 0;
		int srcW = FreeImage_GetWidth(imgPiece);
		int srcH = FreeImage_GetHeight(imgPiece);
		unsigned pitch = FreeImage_GetPitch(imgPiece);
		unsigned destpitch = FreeImage_GetPitch(result);
		BYTE* bits = (BYTE*)FreeImage_GetBits(imgPiece);
		BYTE* destBits = (BYTE*)FreeImage_GetBits(result);
		Vec2 DestPos = CenterPos;
		DestPos.x += lpi->topLeft.x;
		//DestPos.y -= lpi->topLeft.y;
		DestPos.y = OutputSize.y - srcH;
		DestPos.y -= CenterPos.y;
		DestPos.y += lpi->topLeft.y;
		DestPos.x = (unsigned int)(DestPos.x);
		DestPos.y = ceil(DestPos.y);
		for(int y = 0; y < srcH; y++)
		{
			BYTE* pixel = bits;
			BYTE* destpixel = destBits;
			destpixel += (unsigned)((DestPos.y + y + 3)) * destpitch;
			destpixel += (unsigned)((DestPos.x + 3) * 4);
			for(int x = 0; x < srcW; x++)
			{
				destpixel[FI_RGBA_RED] = pixel[FI_RGBA_RED];
				destpixel[FI_RGBA_GREEN] = pixel[FI_RGBA_GREEN];
				destpixel[FI_RGBA_BLUE] = pixel[FI_RGBA_BLUE];
				//if(pixel[FI_RGBA_ALPHA] != 0)
					destpixel[FI_RGBA_ALPHA] = pixel[FI_RGBA_ALPHA];
				pixel += 4;
				destpixel += 4;
			}
			bits += pitch;
		}
		
		FreeImage_Unload(imgPiece);
	}
	FreeImage_Unload(curImg);
	
	return result;
}


int splitImages(const char* cFilename)
{
	vector<uint8_t> vData;
	FILE* fh = fopen( cFilename, "rb" );
	if(fh == NULL)
	{
		cerr << "Unable to open input file " << cFilename << endl;
		return 1;
	}
	fseek( fh, 0, SEEK_END );
	size_t fileSize = ftell( fh );
	fseek( fh, 0, SEEK_SET );
	vData.reserve( fileSize );
	size_t amt = fread( vData.data(), fileSize, 1, fh );
	fclose( fh );
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
	
	//Parse through, splitting out before each ZLFW header
	int iCurFile = 0;
	uint64_t startPos = 0;
	for(uint64_t i = 0; i < fileSize; i++)	//Definitely not the fastest way to do it... but I don't care
	{
		if(memcmp ( &(vData.data()[i]), "ZLFW", 4 ) == 0)	//Found another file
		{
			//Get texture header
			uint64_t headerPos = i - sizeof(texHeader);
			texHeader th;
			memcpy(&th, &(vData.data()[headerPos]), sizeof(texHeader));
			
			//if(th.type != TEXTURE_TYPE_DXT1_COL_MUL)	//Type of image we don't support; skip
			//{
			//	cout << "Tex header: type=" << th.type << ", width=" << th.width << ", height=" << th.height << endl;
				//continue;
			//}
				
			//Search for frame header if we're going to be piecing these together
			FrameDesc fd;
			if(g_bPieceTogether)
			{
				if(iCurFile == 0)	//First one tells us where to start searching backwards from
					startPos = i;
				for(int k = startPos - sizeof(texHeader) - sizeof(FrameDesc); k > 0; k -= 4)
				{
					memcpy(&fd, &(vData.data()[k]), sizeof(FrameDesc));
					if(fd.texOffset != headerPos) continue;
					//Sanity check
					if(fd.texDataSize > 6475888) continue;	//TODO: Test to make sure data size matches
					if(fd.texOffset == 0 || fd.pieceOffset == 0) continue;
					
					//Ok, found our header
					break;
				}
			}
			
			//Decompress WFLZ data
			uint32_t* chunk = NULL;
			const uint32_t decompressedSize = wfLZ_GetDecompressedSize( &(vData.data()[i]) );
			uint8_t* dst = ( uint8_t* )malloc( decompressedSize );
			uint32_t offset = 0;
			int count = 0;
			while( uint8_t* compressedBlock = wfLZ_ChunkDecompressLoop( &(vData.data())[i], &chunk ) )
			{		
				wfLZ_Decompress( compressedBlock, dst + offset );
				const uint32_t blockSize = wfLZ_GetDecompressedSize( compressedBlock );
				offset += blockSize;
			}
			
			//Decompress image
			uint8_t* color = NULL;
			uint8_t* mul = NULL;
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
			
			if(g_bPieceTogether)
			{
				PiecesDesc pd;
				memcpy(&pd, &(vData.data()[fd.pieceOffset]), sizeof(PiecesDesc));
				for(uint32_t j = 0; j < pd.numPieces; j++)
				{
					piece p;
					memcpy(&p, &(vData.data()[fd.pieceOffset+j*sizeof(piece)+sizeof(PiecesDesc)]), sizeof(piece));
					//Store our maximum values, so we know how large the image is
					if(p.topLeft.x < maxul.x)
						maxul.x = p.topLeft.x;
					if(p.topLeft.y > maxul.y)
						maxul.y = p.topLeft.y;
					if(p.bottomRight.x > maxbr.x)
						maxbr.x = p.bottomRight.x;
					if(p.bottomRight.y < maxbr.y)
						maxbr.y = p.bottomRight.y;
					pieces.push_back(p);
				}
			}
			
			uint8_t* dest_final = NULL;
			if(color != NULL && mul != NULL)
			{
				dest_final = (uint8_t*)malloc(decompressedSize * 8);
				multiply(dest_final, color, mul, th);
				free(color);
				free(mul);
			}
			else if(color != NULL)
				dest_final = color;
			else if(mul != NULL)
				dest_final = mul;
			else	//Should be unreachable
			{
				free(dst);
				continue;
			}
			
			FIBITMAP* result = NULL;
			if(g_bPieceTogether)
				result = PieceImage(dest_final, pieces, maxul, maxbr, th);
			else
				result = imageFromPixels(dest_final, th.width, th.height);
			
			ostringstream oss;
			oss << "output/" << sName << "_" << iCurFile+1 << ".png";
			cout << "Saving " << oss.str() << endl;
			FreeImage_Save(FIF_PNG, result, oss.str().c_str());
			FreeImage_Unload(result);
			
			
			//Merge images together and piece them up
			/*if(!g_bSeparate && g_bPieceTogether)
			{
				uint8_t* dest_final = (uint8_t*)malloc(decompressedSize * 8);
				multiply(dest_final, color, mul, th);
			
				FIBITMAP* result = PieceImage(dest_final, pieces, maxul, maxbr, th);
				
				char cOutName[256];
				sprintf(cOutName, "output/%s_%d.png", sName.c_str(), iCurFile);
				cout << "Saving " << cOutName << endl;
				FreeImage_Save(FIF_PNG, result, cOutName);
				FreeImage_Unload(result);
				free(dest_final);
			}
			//Separate images, pieced together
			else if(g_bSeparate && g_bPieceTogether)
			{
				char cOutName[256];
				//Save color map
				if(!g_bMulOnly)
				{
					FIBITMAP* result = PieceImage(color, pieces, maxul, maxbr, th, false, false);
					sprintf(cOutName, "output/%s_%d-col.png", sName.c_str(), iCurFile);
					cout << "Saving " << cOutName << endl;
					FreeImage_Save(FIF_PNG, result, cOutName);
					FreeImage_Unload(result);
				}
				
				//Save multiply map
				if(!g_bColOnly)
				{
					FIBITMAP* result = PieceImage(mul, pieces, maxul, maxbr, th, true);
					sprintf(cOutName, "output/%s_%d-mul.png", sName.c_str(), iCurFile);
					cout << "Saving " << cOutName << endl;
					FreeImage_Save(FIF_PNG, result, cOutName);
					FreeImage_Unload(result);
				}
			}
			//Separate images, not pieced together
			else if(g_bSeparate && !g_bPieceTogether)
			{
				FIBITMAP* result;
				char cOutName[256];
				
				//Save color image
				if(!g_bMulOnly)
				{
					result = imageFromPixels(color, th.width, th.height);
					sprintf(cOutName, "output/%s_%d-col.png", sName.c_str(), iCurFile);
					cout << "Saving " << cOutName << endl;
					FreeImage_Save(FIF_PNG, result, cOutName);
					FreeImage_Unload(result);
				}
				
				//Save multiply image
				if(!g_bColOnly)
				{
					result = imageFromPixels(mul, th.width, th.height);
					sprintf(cOutName, "output/%s_%d-mul.png", sName.c_str(), iCurFile);
					cout << "Saving " << cOutName << endl;
					FreeImage_Save(FIF_PNG, result, cOutName);
					FreeImage_Unload(result);
				}
			}
			//Same image, not pieced together
			else if(!g_bSeparate && !g_bPieceTogether)
			{
				uint8_t* dest_final = (uint8_t*)malloc(th.width * th.height * 4);
				multiply(dest_final, color, mul, th);
				FIBITMAP* result = imageFromPixels(dest_final, th.width, th.height);
				
				char cOutName[256];
				sprintf(cOutName, "output/%s_%d.png", sName.c_str(), iCurFile);
				cout << "Saving " << cOutName << endl;
				FreeImage_Save(FIF_PNG, result, cOutName);
				FreeImage_Unload(result);
				
				free(dest_final);
			}*/
			//Free allocated memory
			free(dst);
			free(dest_final);
			//free(color);
			//free(mul);
			
			iCurFile++;
		}
	}
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
