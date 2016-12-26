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
#include <fstream>
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
	uint32_t v1;
	uint32_t v2;
	uint32_t v3;
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
	uint32_t w;	//Not sure what this is for
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

double convertFromHiLo(uint32_t hi, uint32_t lo)
{
	uint64_t val = lo;
	val |= (((uint64_t)hi) << 32);
	return *((double*)&val);
}

float reverseFloat( const float inFloat )
{
   float retVal;
   char *floatToConvert = ( char* ) & inFloat;
   char *returnFloat = ( char* ) & retVal;

   // swap the bytes into a temporary buffer
   returnFloat[0] = floatToConvert[3];
   returnFloat[1] = floatToConvert[2];
   returnFloat[2] = floatToConvert[1];
   returnFloat[3] = floatToConvert[0];

   return retVal;
}

vector<vertEntry> extractVertices(uint8_t* fileData, blobVertexData bvd, string filename)
{
	vertDataHeader vdh;
	memcpy(&vdh, &fileData[bvd.offset], sizeof(vertDataHeader));
	
	//string outFilename = filename + ".vertices.txt";
	//ofstream ofile;
	//ofile.open(outFilename.c_str());
	vector<vertEntry> vertices;
	
	for(uint32_t i = 0; i < bvd.count; i++)
	{
		uint32_t offset = bvd.offset+sizeof(vertDataHeader)+i*24;
		
		vertEntry ve;
		//memcpy(&ve, &fileData[offset], sizeof(vertEntry));
		memcpy(&ve.x, &fileData[offset], sizeof(float));
		memcpy(&ve.y, &fileData[offset+4], sizeof(float));
		memcpy(&ve.z, &fileData[offset+8], sizeof(float));
		
		memcpy(&ve.u, &fileData[offset+12], sizeof(float));
		memcpy(&ve.w, &fileData[offset+16], sizeof(uint32_t));
		memcpy(&ve.v, &fileData[offset+20], sizeof(float));
		
		ve.u *= ve.w;//reverseFloat(ve.u);
		ve.v *= ve.w;//reverseFloat(ve.v);
		//memcpy(&second, &fileData[offset+20], sizeof(uint32_t));
		
		//first >>= 16;
		//ve.u = 0.5;//*((double*)&first);
		//ve.v = convertFromHiLo(mid & 0xFFFF, second);
		
		
		//memcpy(&ve.u, &fileData[offset+12], sizeof(double));
		//memcpy(&ve.w, &fileData[offset+16], sizeof(float));
		//memcpy(&ve.v, &fileData[offset+16], sizeof(double));
		
		vertices.push_back(ve);
		//ofile << "v " << ve.x << " " << ve.y << " " << ve.z << endl;
	}
	//ofile.close();
	return vertices;
}

vector<faceIndex> extractFaces(uint8_t* fileData, blobFaceData bfd, string filename)
{
	faceDataHeader fdh;
	memcpy(&fdh, &fileData[bfd.offset], sizeof(faceDataHeader));
	
	//string outFilename = filename + ".faces.txt";
	//ofstream ofile;
	//ofile.open(outFilename.c_str());
	vector<faceIndex> faces;
	
	for(uint32_t i = 0; i < bfd.numIndices / 3; i++)
	{
		faceIndex fi;
		memcpy(&fi, &fileData[bfd.offset+sizeof(faceDataHeader)+i*sizeof(faceIndex)], sizeof(faceIndex));
		
		faces.push_back(fi);
		//ofile << "f " << fi.v1+1 << " " << fi.v2+1 << " " << fi.v3+1 << endl;
	}
	//ofile.close();
	return faces;
}

void outputObj(string filename, vector<vertEntry> vertices, vector<faceIndex> faces)
{
	ofstream ofile;
	ofile.open(filename.c_str());
	
	for(int i = 0; i < vertices.size(); i++)
		ofile << "v " << vertices[i].x << " " << vertices[i].y << " " << vertices[i].z << endl;
	
	for(int i = 0; i < vertices.size(); i++)
		ofile << "vt " << vertices[i].u << " " << vertices[i].v << " " << vertices[i].w << endl;
	
	//ofile << "vn 0.50 0.50" << endl;
	
	//ofile << "s off" << endl;
	
	for(int i = 0; i < faces.size(); i++)
		ofile << "f " 
		      << faces[i].v1+1 << "/" << faces[i].v1+1 << ' ' 
			  << faces[i].v2+1 << "/" << faces[i].v2+1 << ' ' 
			  << faces[i].v3+1 << "/" << faces[i].v3+1 << endl;
	
	ofile.close();
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
	
	//Assume only one per for now...
	vector<vertEntry> vertices;
	vector<faceIndex> faces;
	
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
		} else if(blobHeader.type == BLOB_TYPE_VERTICES) {
			blobVertexData bvd;
			memcpy(&bvd, &fileData[offset.offset + sizeof(blobOffset)], sizeof(blobVertexData));
			
			vertices = extractVertices(fileData, bvd, filename);
		} else if(blobHeader.type == BLOB_TYPE_FACES) {
			blobFaceData bfd;
			memcpy(&bfd, &fileData[offset.offset + sizeof(blobOffset)], sizeof(blobFaceData));
			
			faces = extractFaces(fileData, bfd, filename);
		}
	}
	
	outputObj(filename + ".obj", vertices, faces);
	
	//vector<face> faces;
	//vector<vert> vertices;
	//vector<uv> uvs;
	
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
