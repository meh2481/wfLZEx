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

//------------------------------
// WF3D file format structs
//------------------------------
typedef struct {
	uint8_t sig[4];	//WFSN
	uint32_t pad[4];	//Always 0
	uint32_t numOffsets;	//Number of offsetList objects
	uint32_t offsetListOffset;	//Offset to offsetList
} wf3dHeader;

typedef struct {
	uint64_t offset;	//Offset in file to blobOffset object
} offsetList;

#define BLOB_TYPE_TEXTURE 	0x1
#define BLOB_TYPE_VERTICES	0x2
#define BLOB_TYPE_FACES		0x3

typedef struct {
	uint32_t type;		//One of the blob types above
	uint32_t unk1;		//
	uint64_t pad;		//Always 0
	//Followed by blob data
} blobOffset;

#define IMAGE_TYPE_DXT1	0x31
#define IMAGE_TYPE_DXT5	0x64

typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t pad1;
	uint32_t type;	//One of the image types above
	uint64_t hash;
	uint32_t imageDataOffset;	//Point to imgDataHeader
	uint32_t unkOffset;
	uint32_t filenameOffset;	//Point to filenameHeader
} blobTextureData;

typedef struct {
	uint32_t numIndices;	//divide by 3 for number of faces
	uint32_t type;	//Always 2?
	uint64_t hash;
	uint32_t offset;	//To faceDataHeader
	uint32_t size;
} blobFaceData;

typedef struct {
	uint32_t count;	//of something
	uint32_t pad;	//Always 0?
	uint64_t hash;
	uint32_t offset;	//To vertDataHeader
	uint32_t size;		//Of vertex data
} blobVertexData;

typedef struct {
	uint32_t unk;	//Always FFFFFF00
	uint32_t compressedSize;
	//Followed by compressedSize WFLZ-compressed image
} imgDataHeader;

typedef struct {
	uint32_t unk;	//Always FFFFFF00
	uint32_t size;
	//Followed by faceIndexes
} faceDataHeader;

typedef struct {
	uint32_t face1;
	uint32_t face2;
	uint32_t face3;
} faceIndex;

typedef struct {
	uint32_t unk;	//Always FFFFFF00
	uint32_t filenameLen;
	//Followed by filenameLen characters
} filenameHeader;

typedef struct {
	uint32_t unk;	//Always FFFFFF00
	uint32_t size;
	//Followed by vertEntrys
} vertDataHeader;

typedef struct {
	float x;
	float y;
	float z;
	float u;
	float v;
	float w;	//Not sure what this is for
} vertEntry;

//------------------------------

FIBITMAP* imageFromPixels(uint8_t* imgData, uint32_t width, uint32_t height)
{
	if(!imgData)
		return FreeImage_Allocate(0,0,32);

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

void extractTexture(uint8_t* fileData, blobTextureData texData)
{
	//Grab the filename
	filenameHeader fnh;
	memcpy(&fnh, &fileData[texData.filenameOffset], sizeof(filenameHeader));
	
	string filename = (const char*) &fileData[texData.filenameOffset + sizeof(filenameHeader)];
	//cout << "Filename for image is " << filename << endl;
	//cout << "Decompressing image at " << texData.imageDataOffset << endl;
	
	//Grab image data header
	imgDataHeader idh;
	memcpy(&idh, &fileData[texData.imageDataOffset], sizeof(imgDataHeader));
	
	//cout << "Compressed size: " << idh.compressedSize << endl;
	uint32_t dataOffset = texData.imageDataOffset + sizeof(imgDataHeader);
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
	
	//Decompress squished image (Assume DXT1 for now)
	uint8_t* imgData = (uint8_t*)malloc(decompressedSize * 8);
	int flags = squish::kDxt1;
	if(texData.type == IMAGE_TYPE_DXT1)
		flags = squish::kDxt1;
	else if(texData.type == IMAGE_TYPE_DXT5)
		flags = squish::kDxt5;
	else
		cout << "Unknown flags " << texData.type << " for image. Assuming DXT1..." << endl;
	squish::DecompressImage(imgData, texData.width, texData.height, dst, flags);
	
	//Save image as PNG
	string outputFilename = filename + ".png";
	cout << "Saving image " << outputFilename << endl;
	FIBITMAP* result = imageFromPixels(imgData, texData.width, texData.height);
	FreeImage_FlipVertical(result);	//These seem to always be upside-down
	FreeImage_Save(FIF_PNG, result, outputFilename.c_str());
	FreeImage_Unload(result);
	
	free(dst);
	free(imgData);
}

void decomp(string filename) 
{
	FILE* fh = fopen(filename.c_str(), "rb");
	if(fh == NULL)
	{
		cerr << "Unable to open input file " << filename << endl;
		return;
	}
	fseek(fh, 0, SEEK_END);
	size_t fileSize = ftell(fh);
	fseek(fh, 0, SEEK_SET);
	uint8_t* fileData = (uint8_t*)malloc(fileSize);
	size_t amt = fread(fileData, fileSize, 1, fh);
	fclose(fh);
	cout << "Parsing WF3D file " << filename << endl;
	
	//Read in file header
	wf3dHeader header;
	memcpy(&header, fileData, sizeof(wf3dHeader));
	if(header.sig[0] != 'W' || header.sig[1] != 'F' || header.sig[2] != 'S' || header.sig[3] != 'N')
	{
		cout << "Error: Not a WF3D file " << filename << endl;
		return;
	}
	
	//Read the offset list
	for(uint32_t curOffset = 0; curOffset < header.numOffsets; curOffset++)
	{
		//Read in this offset
		offsetList offset;
		memcpy(&offset, &fileData[header.offsetListOffset+curOffset*sizeof(offsetList)], sizeof(offsetList));
		
		//Seek to this position and read blob header
		blobOffset blobHeader;
		memcpy(&blobHeader, &fileData[offset.offset], sizeof(blobOffset));
		
		cout << "Found blob of type " << blobHeader.type << " at offset " << offset.offset << endl;
		//Read blob data based on blob type
		if(blobHeader.type == BLOB_TYPE_TEXTURE)
		{
			//Read in texture data header
			blobTextureData texData;
			memcpy(&texData, &fileData[offset.offset + sizeof(blobOffset)], sizeof(blobTextureData));
			
			//Extract texture
			extractTexture(fileData, texData);
		}
	}
	
	//Cleanup
	free(fileData);
}

int main(int argc, char** argv)
{
	list<string> sFilenames;
	//Parse commandline
	for(int i = 1; i < argc; i++)
	{
		string s = argv[i];
		sFilenames.push_back(s);
	}
	//Decompress files
	for(list<string>::iterator i = sFilenames.begin(); i != sFilenames.end(); i++)
		decomp(*i);
	
	return 0;
}
