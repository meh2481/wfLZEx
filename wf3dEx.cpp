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

#define MAJOR 0
#define MINOR 1

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

#define VERT_DATA_WEIGHT_UV 	0
#define VERT_DATA_WEIGHT_TAN_UV	1

typedef struct {
	uint32_t count;	//of something
	uint32_t flags;	//one of above VERT_DATA flags
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
	uint8_t weights[4];
	uint8_t indices[4];
	uint16_t u;
	uint16_t v;
} vertEntryWeightUV;

typedef struct {
	float x;
	float y;
	float z;
	uint8_t weights[4];
	uint8_t indices[4];
	uint32_t tangent[2];
	uint16_t u;
	uint16_t v;
} vertEntryWeightTanUV;

//------------------------------
// Helper classes
//------------------------------

typedef struct {
	float x;
	float y;
	float z;
	float u;
	float v;
} vertHelper;

//------------------------------

#define MAN_BITMASK 0x3FF
#define EX_MAX 0x70

//Convert a half-float to a full float value
float halfFloat(uint16_t in)
{
	uint32_t man = in & MAN_BITMASK;		//Mantissa is lower 10 bits
    uint32_t ex = -EX_MAX;	//Negative unsigned bitwise magic
	if(in & 0x7C00)				//Exponent is upper 6 bits minus topmost for sign
		ex = (in >> 10) & 0x1F;
	else if(man) 
	{
		man <<= 1;
		for(ex = 0; !(man & 0x400); ex--)
			man <<= 1;
		man &= MAN_BITMASK;
	}
	uint32_t tmp = ((in & 0x8000) << 16) | ((ex + EX_MAX) << 23) | (man << 13);
	return *(float*)&tmp;
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

void extractTexture(uint8_t* fileData, blobTextureData texData)
{
	//Grab the filename
	filenameHeader fnh;
	memcpy(&fnh, &fileData[texData.filenameOffset], sizeof(filenameHeader));
	
	string filename = (const char*) &fileData[texData.filenameOffset + sizeof(filenameHeader)];
	
	//Grab image data header
	imgDataHeader idh;
	memcpy(&idh, &fileData[texData.imageDataOffset], sizeof(imgDataHeader));
	
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

vector<vertHelper> extractVertices(uint8_t* fileData, blobVertexData bvd, string filename)
{
	vertDataHeader vdh;
	memcpy(&vdh, &fileData[bvd.offset], sizeof(vertDataHeader));
	
	vector<vertHelper> vertices;
	
	for(uint32_t i = 0; i < bvd.count; i++)
	{
		if(bvd.flags == VERT_DATA_WEIGHT_UV)
		{
			uint32_t offset = bvd.offset+sizeof(vertDataHeader)+i*sizeof(vertEntryWeightUV);
			
			vertEntryWeightUV ve;
			memcpy(&ve, &fileData[offset], sizeof(vertEntryWeightUV));
			
			vertHelper vh;
			vh.x = ve.x;
			vh.y = ve.y;
			vh.z = ve.z;
			vh.u = halfFloat(ve.u);
			vh.v = halfFloat(ve.v);
			
			vertices.push_back(vh);
		}
		else if(bvd.flags == VERT_DATA_WEIGHT_TAN_UV)
		{
			uint32_t offset = bvd.offset+sizeof(vertDataHeader)+i*sizeof(vertEntryWeightTanUV);
			
			vertEntryWeightTanUV ve;
			memcpy(&ve, &fileData[offset], sizeof(vertEntryWeightTanUV));
			
			vertHelper vh;
			vh.x = ve.x;
			vh.y = ve.y;
			vh.z = ve.z;
			vh.u = halfFloat(ve.u);
			vh.v = halfFloat(ve.v);
			
			vertices.push_back(vh);
		}
		else
			cout << "ERR: Unknown vert type: " << bvd.flags << endl;
	}
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

void outputObj(string filename, vector<vector<vertHelper> > vertices, vector<vector<faceIndex> > faces)
{
	if(!vertices.size() || !faces.size())
		return; //Nothing to do
	
	if(vertices.size() != faces.size())
	{
		cout << "ERR: vertices/faces mismatch in size" << endl;
		return;
	}
	
	cout << "Saving obj " << filename << endl;
	
	ofstream ofile;
	ofile.open(filename.c_str());
	
	ofile << "# Created with wf3dEx v" << MAJOR << '.' << MINOR << endl;
	
	int curAdd = 1;
	
	for(int j = 0; j < faces.size(); j++)
	{
		ofile << "o " << filename << '_' << j+1 << endl;
	
		for(int i = 0; i < vertices[j].size(); i++)
			ofile << "v " << vertices[j][i].x << " " << vertices[j][i].y << " " << vertices[j][i].z << endl;
		
		//TODO Other things than uv; there's bones and other such stuff here also
		for(int i = 0; i < vertices[j].size(); i++)
			ofile << "vt " << vertices[j][i].u << " " << vertices[j][i].v << endl;
		
		for(int i = 0; i < faces[j].size(); i++)
			ofile << "f " 
				  << faces[j][i].v1+curAdd << "/" << faces[j][i].v1+curAdd << ' ' 
				  << faces[j][i].v2+curAdd << "/" << faces[j][i].v2+curAdd << ' ' 
				  << faces[j][i].v3+curAdd << "/" << faces[j][i].v3+curAdd << endl;
		
		ofile << endl;
		
		curAdd += vertices[j].size();
	}
	
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
	vector<vector<vertHelper> > vertices;
	vector<vector<faceIndex> > faces;
	
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
			
			//Seems to go in order faces-vertices-faces-vertices
			vertices.push_back(extractVertices(fileData, bvd, filename));
		} else if(blobHeader.type == BLOB_TYPE_FACES) {
			blobFaceData bfd;
			memcpy(&bfd, &fileData[offset.offset + sizeof(blobOffset)], sizeof(blobFaceData));
			
			faces.push_back(extractFaces(fileData, bfd, filename));
		}
	}
	
	outputObj(filename + ".obj", vertices, faces);
	
	//Cleanup
	free(fileData);
}

void print_usage()
{
	cout << "wf3dEx v" << MAJOR << '.' << MINOR << endl << endl;
	cout << "Usage: wf3dEx [wf3d files]" << endl;
}

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		print_usage();
		return 0;
	}
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
