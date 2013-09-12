#include "wfLZ.h"
#include <iostream>
#include <vector>
#include <string>
#include <stdlib.h>
#include <cstdio>
#include <squish.h>
#include <windows.h>
#include "FreeImage.h"
#include <list>
#include <cmath>
using namespace std;

typedef struct
{
	uint32_t unknown0;
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
	fread( vData.data(), fileSize, 1, fh );
	fclose( fh );
	
	//Parse through, splitting out before each ZLFW header
	int iCurFile = 0;
	uint64_t startPos = 0;
	for(uint64_t i = 0; i < fileSize; i++)	//Definitely not the fastest way to do it... but I don't care
	{
		if(memcmp ( &(vData.data()[i]), "ZLFW", 4 ) == 0)	//Found another file
		{
			uint64_t headerPos = i - sizeof(texHeader);
			texHeader th;
			memcpy ( &th, &(vData.data()[headerPos]), sizeof(texHeader) );
			if(iCurFile == 0)	//First one
				startPos = i;
			FrameDesc fd;
			for(int k = startPos - sizeof(texHeader) - sizeof(FrameDesc); k > 0; k -= 4)
			{
				memcpy(&fd, &(vData.data()[k]), sizeof(FrameDesc));
				if(fd.texOffset != headerPos) continue;
				//Sanity check
				if(fd.texDataSize > 6475888) continue;
				if(fd.texOffset == 0 || fd.pieceOffset == 0) continue;
				
				//Ok, found our header
				break;
			}
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
			
			uint8_t* color = (uint8_t*)malloc(decompressedSize * 8);
			
			squish::DecompressImage( color, th.width, th.height, dst, squish::kDxt1 );
			
			//Create second image
			uint8_t* mul = (uint8_t*)malloc(decompressedSize * 8);
			squish::DecompressImage( mul, th.width, th.height, dst + decompressedSize/2, squish::kDxt1 );	//Second image starts halfway through decompressed data
			
			
			uint8_t* dest_final = (uint8_t*)malloc(decompressedSize * 8);
			//Multiply together
			for(int j = 0; j < th.width * th.height * 4; j+=4)
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
			
			//Read in pieces
			PiecesDesc pd;
			Vec2 maxul;
			Vec2 maxbr;
			maxul.x = maxul.y = maxbr.x = maxbr.y = 0;
			memcpy(&pd, &(vData.data()[fd.pieceOffset]), sizeof(PiecesDesc));
			list<piece> pieces;
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
			
			Vec2 OutputSize;
			Vec2 CenterPos;
			OutputSize.x = -maxul.x + maxbr.x;
			OutputSize.y = maxul.y - maxbr.y;
			CenterPos.x = -maxul.x;
			CenterPos.y = maxul.y;
			OutputSize.x = uint32_t(OutputSize.x);
			OutputSize.y = uint32_t(OutputSize.y);
			
			FIBITMAP* result = FreeImage_Allocate(OutputSize.x+6, OutputSize.y+6, 32);
			
			//Create image from this set of pixels
			FIBITMAP* curImg = FreeImage_Allocate(th.width, th.height, 32);
			FREE_IMAGE_TYPE image_type = FreeImage_GetImageType(curImg);
			if(image_type == FIT_BITMAP)
			{
				int curPos = 0;
				unsigned pitch = FreeImage_GetPitch(curImg);
				BYTE* bits = (BYTE*)FreeImage_GetBits(curImg);
				bits += pitch * th.height - pitch;
				for(int y = th.height-1; y >= 0; y--)
				{
					BYTE* pixel = (BYTE*)bits;
					for(int x = 0; x < th.width; x++)
					{
						pixel[FI_RGBA_RED] = dest_final[curPos++];
						pixel[FI_RGBA_GREEN] = dest_final[curPos++];
						pixel[FI_RGBA_BLUE] = dest_final[curPos++];
						pixel[FI_RGBA_ALPHA] = dest_final[curPos++];
						pixel += 4;
					}
					bits -= pitch;
				}
			}
			
			//Patch image together from pieces
			for(list<piece>::iterator lpi = pieces.begin(); lpi != pieces.end(); lpi++)
			{
				FIBITMAP* imgPiece = FreeImage_Copy(curImg, 
													floor((lpi->topLeftUV.x) * th.width - 0.001), floor((lpi->topLeftUV.y) * th.height - 0.001), 
													ceil((lpi->bottomRightUV.x) * th.width + 0.001), ceil((lpi->bottomRightUV.y) * th.height + 0.001));
				
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
			
			free(dst);
			free(color);
			free(mul);
			free(dest_final);
			char cOutName[256];
			sprintf(cOutName, "output/%s-%d.png", cFilename, ++iCurFile);
			FreeImage_Save(FIF_PNG, result, cOutName);
			FreeImage_Unload(result);
			FreeImage_Unload(curImg);
			
			//TODO: Remove. For testing purposes, we'll only do one image because faster
			//break;
		}
	}
	return 0;
}

int main(int argc, char** argv)
{
	FreeImage_Initialise();
	CreateDirectory(TEXT("output"), NULL);
	for(int i = 1; i < argc; i++)
	{
		splitImages(argv[i]);
	}
	FreeImage_DeInitialise();
	return 0;
}
