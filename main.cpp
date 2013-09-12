#include "wfLZ.h"
#include <iostream>
#include <vector>
#include <string>
#include <stdlib.h>
#include <cstdio>
#include <squish.h>
#include "png.h"
#include <windows.h>
using namespace std;

bool convertToPNG(const char* cFilename, uint8_t* data, uint32_t w, uint32_t h)
{
  FILE          *png_file;
  png_struct    *png_ptr = NULL;
  png_info      *info_ptr = NULL;
  png_byte      *png_pixels = NULL;
  png_byte      **row_pointers = NULL;
  png_uint_32   row_bytes;

  int           color_type = PNG_COLOR_TYPE_RGB_ALPHA;
  int           bit_depth = 8;
  int           channels = 4;
  
  png_file = fopen(cFilename, ("wb"));
  if(png_file == NULL)
  {
    cout << "PNG file " << (cFilename) << " NULL" << endl;
	return false;
  }
  setvbuf ( png_file , NULL , _IOFBF , 4096 );
  
  //Read in the image
  size_t sizeToRead = w * h * channels * bit_depth/8;
  png_pixels = data;

  // prepare the standard PNG structures 
  png_ptr = png_create_write_struct (PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!png_ptr)
  {
    cout << "png_ptr Null" << endl;
	fclose(png_file);
	return false;
  }
	
  info_ptr = png_create_info_struct (png_ptr);
  if (!info_ptr)
  {
    cout << "Info ptr null" << endl;
    png_destroy_write_struct (&png_ptr, (png_infopp) NULL);
	fclose(png_file);
	return false;
  }

  // setjmp() must be called in every function that calls a PNG-reading libpng function
  if (setjmp (png_jmpbuf(png_ptr)))
  {
    cout << "unable to setjmp" << endl;
    png_destroy_write_struct (&png_ptr, (png_infopp) NULL);
	fclose(png_file);
	return false;
  }

  // initialize the png structure
  png_init_io (png_ptr, png_file);

  // we're going to write more or less the same PNG as the input file
  png_set_IHDR (png_ptr, info_ptr, w, h, bit_depth, color_type, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

  // write the file header information
  png_write_info (png_ptr, info_ptr);

  // if needed we will allocate memory for an new array of row-pointers
  if (row_pointers == (png_byte**) NULL)
  {
    if ((row_pointers = (png_byte **) malloc (h * sizeof (png_bytep))) == NULL)
    {
      png_destroy_write_struct (&png_ptr, (png_infopp) NULL);
	  cout << "Error allocating row pointers" << endl;
	  fclose(png_file);
	  return false;
    }
  }

  // row_bytes is the width x number of channels x (bit-depth / 8)
  row_bytes = w * channels * ((bit_depth <= 8) ? 1 : 2);
  
  // set the individual row_pointers to point at the correct offsets
  for (unsigned int i = 0; i < (h); i++)
    row_pointers[i] = png_pixels + i * row_bytes;

  // write out the entire image data in one call
  png_write_image (png_ptr, row_pointers);

  // write the additional chuncks to the PNG file (not really needed)
  png_write_end (png_ptr, info_ptr);

  // clean up after the write, and free any memory allocated
  png_destroy_write_struct (&png_ptr, (png_infopp) NULL);

  if (row_pointers != (png_byte**) NULL)
    free (row_pointers);
	
  //Close the file
  fclose(png_file);

  return true;
}


typedef struct
{
	uint32_t imgType;
	uint32_t imgW;
	uint32_t imgH;
	uint32_t mipFlags;	//0 means no mips, 1 means that this block contains two images
	uint32_t mipPos[4];	//offset for texture comes later
} anbHeader;

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
	for(int i = 0; i < fileSize; i++)	//Definitely not the fastest way to do it... but I don't care
	{
		if(memcmp ( &(vData.data()[i]), "ZLFW", 4 ) == 0)	//Found another file
		{
			int headerPos = i - sizeof(anbHeader);
			anbHeader ah;
			memcpy ( &ah, &(vData.data()[headerPos]), sizeof(anbHeader) );
			
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
			
			squish::DecompressImage( color, ah.imgW, ah.imgH, dst, squish::kDxt1 );
			char cOutName[256];
			sprintf(cOutName, "output/%s-%d.png", cFilename, ++iCurFile);
			
			//Create second image
			uint8_t* mul = (uint8_t*)malloc(decompressedSize * 8);
			squish::DecompressImage( mul, ah.imgW, ah.imgH, dst + decompressedSize/2, squish::kDxt1 );	//Second image starts halfway through decompressed data
			
			
			uint8_t* dest_final = (uint8_t*)malloc(decompressedSize * 8);
			//Multiply together
			for(int j = 0; j < ah.imgW * ah.imgH * 4; j+=4)
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
			
			//Save result to PNG
			convertToPNG(cOutName, dest_final, ah.imgW, ah.imgH);
			
			
			free(dst);
			free(color);
			free(mul);
			free(dest_final);
		}
	}
	return 0;
}

int main(int argc, char** argv)
{
	CreateDirectory(TEXT("output"), NULL);
	for(int i = 1; i < argc; i++)
	{
		splitImages(argv[i]);
	}
	return 0;
}
