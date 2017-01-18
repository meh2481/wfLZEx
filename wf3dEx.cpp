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
#include <map>
using namespace std;

#define MAJOR 0
#define MINOR 1

//------------------------------
// WF3D file format structs
//------------------------------
typedef struct {
	uint8_t sig[4];	//WFSN
	uint32_t pad[3];	//Always 0
	//Followed by root node
} wf3dHeader;

typedef struct {
	uint64_t offset;	//Offset in file to node object
} offsetList;

#define NODE_TYPE_ROOT		0x0
#define NODE_TYPE_TEXTURE 	0x1
#define NODE_TYPE_VERTICES	0x2
#define NODE_TYPE_FACES		0x3
#define NODE_TYPE_OBJ_TEXTURE_MAP 0x4
#define NODE_TYPE_GROUP		0x5
#define NODE_TYPE_OBJ_MAP	0x6
#define NODE_TYPE_BONE_NAME	0x8
#define NODE_TYPE_BONES		0x9
#define NODE_TYPE_COLLISION	0xA
#define NODE_TYPE_OBJDATA	0xB

typedef struct {
	uint32_t type;				//One of the node types above
	uint32_t numChildren;		//Number of children in offsetList
	uint64_t childListOffset;	//Offset to offsetList
	//Followed by one of the Node data types below
} Node;

typedef struct {
	uint32_t offset;
	uint32_t size;
} Offset32;		//Used in a lot of headers

#define IMAGE_TYPE_DXT1	0x31
#define IMAGE_TYPE_DXT5	0x64

typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t unk1;
	uint32_t type;	//One of the image types above
	uint64_t hash;
	uint32_t imageDataOffset;	//Point to DataHeader
	uint32_t unk2;
	uint32_t filenameOffset;	//Point to DataHeader
} TextureNode;

typedef struct {
	uint32_t numIndices;	//divide by 3 for number of faces
	uint32_t type;	//Always 2?
	uint64_t hash;
	Offset32 offset;	//To DataHeader
} FaceNode;

typedef struct {
	uint64_t objHash;
	uint64_t texHash;
	uint64_t unk[3];
} ObjTextureNode;

typedef struct {
	uint64_t unkHash1;
	uint64_t objHash;	//Matches ObjTextureNode objHash
	uint64_t vertHash;	//Matches VertexNode hash
	uint64_t faceHash;	//Matches FaceNode hash
	float unk[6];
} ObjMapNode;

typedef struct {
	uint32_t unk[2];
	Offset32 offset;	//Offset to DataHeader
} CollisionNode;

typedef struct {
	uint32_t unk1;
	uint32_t flags;
	Offset32 dataOffset1;
	Offset32 dataOffset2;
	Offset32 dataNameOffset;
} ObjDataNode;

typedef struct {
	uint32_t unk1;
	uint32_t numBones;
	Offset32 offset;	//To DataHeader followed by numBones BoneMatrix entries
} BoneNode;

#define VERT_DATA_WEIGHT_UV 	0
#define VERT_DATA_WEIGHT_TAN_UV	1

typedef struct {
	uint32_t count;	//of something
	uint32_t flags;	//one of above VERT_DATA flags
	uint64_t hash;
	Offset32 offset;	//To DataHeader
} VertexNode;

typedef struct {
	uint32_t unk1;	//Random or something
	uint32_t unk2;	//Always FFFFFFFF?
	uint32_t idx[4];	//Unknown
	Offset32 unkOffset;
	Offset32 boneNameOffset;
} BoneNameNode;

//Generic header used for storing data
typedef struct {
	uint32_t unk;	//Always FFFFFF00?
	uint32_t size;	//bytes
	//Followed by data
} DataHeader;

typedef struct {
	uint32_t v1;
	uint32_t v2;
	uint32_t v3;
} faceIndex;

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

typedef struct {
	float val[16];	//4x4 matrix probably
} BoneMatrix;

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

string stripExtension(string filename)
{
	//Erase extension from end
	size_t pos = filename.rfind('.');
	if(pos != string::npos)
		filename.erase(pos);
	

	
	return filename;
}

string stripPath(string filename)
{
	//Next, strip off any file path before it
	size_t pos = filename.rfind('/');	//Forward slash (UNIX-style)
	if(pos == string::npos)
		pos = filename.rfind('\\');	//Backslash (Windows-style)
	if(pos != string::npos)
		filename.erase(0, pos+1);
	
	return filename;
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

string extractTexture(uint8_t* fileData, TextureNode texData)
{
	//Grab the filename
	DataHeader sh;
	memcpy(&sh, &fileData[texData.filenameOffset], sizeof(DataHeader));
	
	string filename = (const char*) &fileData[texData.filenameOffset + sizeof(DataHeader)];
	
	//Grab image data header
	DataHeader idh;
	memcpy(&idh, &fileData[texData.imageDataOffset], sizeof(DataHeader));
	
	uint32_t dataOffset = texData.imageDataOffset + sizeof(DataHeader);
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
	string outputFilename = stripExtension(filename) + ".png";	//Convert .tga or .psd to .png
	cout << "Saving image " << outputFilename << endl;
	FIBITMAP* result = imageFromPixels(imgData, texData.width, texData.height);
	FreeImage_FlipVertical(result);	//These seem to always be upside-down
	FreeImage_Save(FIF_PNG, result, outputFilename.c_str());
	FreeImage_Unload(result);
	
	free(dst);
	free(imgData);
	
	return outputFilename;
}

vector<vertHelper> extractVertices(uint8_t* fileData, VertexNode bvd)
{
	DataHeader vdh;
	memcpy(&vdh, &fileData[bvd.offset.offset], sizeof(DataHeader));
	
	vector<vertHelper> vertices;
	
	for(uint32_t i = 0; i < bvd.count; i++)
	{
		if(bvd.flags == VERT_DATA_WEIGHT_UV)
		{
			uint32_t offset = bvd.offset.offset+sizeof(DataHeader)+i*sizeof(vertEntryWeightUV);
			
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
			uint32_t offset = bvd.offset.offset+sizeof(DataHeader)+i*sizeof(vertEntryWeightTanUV);
			
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

vector<faceIndex> extractFaces(uint8_t* fileData, FaceNode bfd)
{	
	DataHeader fdh;
	memcpy(&fdh, &fileData[bfd.offset.offset], sizeof(DataHeader));
	vector<faceIndex> faces;
	
	for(uint32_t i = 0; i < bfd.numIndices / 3; i++)
	{
		faceIndex fi;
		memcpy(&fi, &fileData[bfd.offset.offset+sizeof(DataHeader)+i*sizeof(faceIndex)], sizeof(faceIndex));
		
		faces.push_back(fi);
	}
	return faces;
}

vector<BoneMatrix> extractBones(uint8_t* fileData, BoneNode bn)
{
	vector<BoneMatrix> ret;
	//FILE* fp = fopen("out.bones", "wb");
	//fwrite((void*)&bn.numBones, 1, sizeof(uint32_t), fp);
	for(uint32_t i = 0; i < bn.numBones; i++) 
	{
		BoneMatrix bm;
		memcpy(&bm, &fileData[bn.offset.offset+i*sizeof(BoneMatrix)+sizeof(DataHeader)], sizeof(BoneMatrix));
		
		ret.push_back(bm);
		//TODO Store matrix
		/*cerr << "matrix = mathutils.Matrix([[" << bm.val[0] << ", " << bm.val[1] << ", " << bm.val[2] << ", " << bm.val[3] << "]," 
			 << "[" << bm.val[4] << ", " << bm.val[5] << ", " << bm.val[6] << ", " << bm.val[7] << "],"  
			 << "[" << bm.val[8] << ", " << bm.val[9] << ", " << bm.val[10] << ", " << bm.val[11] << "],"  
			 << "[" << bm.val[12] << ", " << bm.val[13] << ", " << bm.val[14] << ", " << bm.val[15] << "]]).inverted()" << endl;
		
		cerr << "bone = armature.edit_bones.new('bone" << i << "')" << endl
			 << "bone.tail = mathutils.Vector([1,0,0])" << endl
			 << "bone.transform(matrix)" << endl;*/
			 
		//for(int j = 0; j < 16; j++)
			//fwrite(&fileData[bn.offset.offset+i*sizeof(BoneMatrix)+sizeof(DataHeader)], 1, sizeof(BoneMatrix), fp);
		
	}
	//fclose(fp);
	return ret;
}

string extractBoneName(uint8_t* fileData, BoneNameNode bnn)
{
	string s = (const char*)&fileData[bnn.boneNameOffset.offset+sizeof(DataHeader)];
	//cout << "bone name: " << s << endl;
	return s;
}

//Global vars for use while decompressing
map<uint64_t, string> textureFilenames;
map<uint64_t, uint64_t> objTextureMap;
map<uint64_t, vector<faceIndex> > faces;
map<uint64_t, vector<vertHelper> > vertices;
vector<ObjMapNode> objects;
vector<vector<BoneMatrix> > bones;
vector<string> boneNames;

void outputObj(string filename)
{
	//Convert .wf3d extension to .obj
	filename = stripExtension(filename);
	string name = stripPath(filename);	//Get base name while we're at it
	string objFilename = filename + ".obj";
	string mtlFilename = filename + ".mtl";
	
	cout << "Saving obj " << objFilename << endl;
	cout << "Saving mtl " << mtlFilename << endl;
	
	ofstream ofile;
	ofile.open(objFilename.c_str());
	
	ofstream mfile;
	mfile.open(mtlFilename.c_str());
	
	ofile << "# Created with wf3dEx v" << MAJOR << '.' << MINOR << endl;
	mfile << "# Created with wf3dEx v" << MAJOR << '.' << MINOR << endl;
	
	int curAdd = 1;
	for(int i = 0; i < objects.size(); i++)
	{
		ObjMapNode omn = objects[i];
		
		//Create mtl for each texture and stuff
		mfile << "newmtl " << i << endl
			  << "map_Kd " << textureFilenames[objTextureMap[omn.objHash]] << endl;
		
		ofile << "mtllib " << mtlFilename << endl;
		
		//wavefront supports multiple objects; use them per obj we get
		ofile << "o " << name << '_' << i+1 << endl;
		
		//Vertices
		vector<vertHelper> vertList = vertices[omn.vertHash];
		for(int j = 0; j < vertList.size(); j++)
			ofile << "v " << vertList[j].x << " " << vertList[j].y << " " << vertList[j].z << endl;
		
		//Vertex UV coordinates
		for(int j = 0; j < vertList.size(); j++)
			ofile << "vt " << vertList[j].u << " " << vertList[j].v << endl;
		
		//Faces
		ofile << "usemtl " << i << endl;
		vector<faceIndex> faceList = faces[omn.faceHash];
		//Wind faces in order 3-2-1 so Blender outputs correct vertex normals
		for(int j = 0; j < faceList.size(); j++)
			ofile << "f " 
				  << faceList[j].v3+curAdd << "/" << faceList[j].v3+curAdd << ' '	//UV and vertex indices are the same
				  << faceList[j].v2+curAdd << "/" << faceList[j].v2+curAdd << ' ' 
				  << faceList[j].v1+curAdd << "/" << faceList[j].v1+curAdd << endl;
		
		ofile << endl;
		
		curAdd += vertList.size();
	}
	
	ofile.close();
	mfile.close();
	
	//Spit out bones
	for(int i = 0; i < bones.size(); i++)
	{
		ostringstream oss;
		oss << filename << i+1 << ".bones";
		cout << "Saving bones " << oss.str() << endl;
		FILE* fp = fopen(oss.str().c_str(), "wb");
		uint32_t sz = bones[i].size();
		fwrite(&sz, 1, sizeof(uint32_t), fp);
		
		for(int j = 0; j < bones[i].size(); j++)
			fwrite(&bones[i][j], 1, sizeof(BoneMatrix), fp);
		
		fclose(fp);
	}
	
	//Spit out bone names
	ostringstream oss;
	oss << filename << ".bonenames";
	cout << "Saving bone names " << oss.str() << endl;
	FILE* fp = fopen(oss.str().c_str(), "wb");
	uint32_t sz = boneNames.size();
	fwrite(&sz, 1, sizeof(uint32_t), fp);
	for(uint32_t i = 0; i < sz; i++)
	{
		uint32_t len = boneNames[i].size();
		fwrite(&len, 1, sizeof(uint32_t), fp);
		for(uint32_t j = 0; j < len; j++)
			fwrite(&boneNames[i][j], 1, 1, fp);
	}
	fclose(fp);
}

void readNode(uint8_t* fileData, uint64_t nodeOffset, int numTabs)
{
	Node node;
	memcpy(&node, &fileData[nodeOffset], sizeof(Node));
	
	//Put number of tabs
	for(int i = 0; i < numTabs; i++)
		cout << '\t';
	cout << "Node of type " << node.type << " at offset " << std::hex << nodeOffset << std::dec << endl;
	
	//Read node data based on node type
	switch(node.type)
	{
		case NODE_TYPE_ROOT:
			break;	//Root node has no data
		
		case NODE_TYPE_TEXTURE:
		{
			TextureNode texNode;
			memcpy(&texNode, &fileData[nodeOffset + sizeof(Node)], sizeof(TextureNode));
			
			//Extract texture
			string textureFilename = extractTexture(fileData, texNode);
			textureFilenames[texNode.hash] = textureFilename;
			break;
		}
		
		case NODE_TYPE_VERTICES:
		{
			VertexNode bvd;
			memcpy(&bvd, &fileData[nodeOffset + sizeof(Node)], sizeof(VertexNode));
			
			vertices[bvd.hash] = extractVertices(fileData, bvd);
			break;
		}	
		
		case NODE_TYPE_FACES:
		{
			FaceNode bfd;
			memcpy(&bfd, &fileData[nodeOffset + sizeof(Node)], sizeof(FaceNode));
			
			faces[bfd.hash] = extractFaces(fileData, bfd);
			break;
		}
		
		case NODE_TYPE_OBJ_TEXTURE_MAP:
		{
			ObjTextureNode otn;
			memcpy(&otn, &fileData[nodeOffset + sizeof(Node)], sizeof(ObjTextureNode));
			
			objTextureMap[otn.objHash] = otn.texHash;
			break;
		}
		
		case NODE_TYPE_OBJ_MAP:
		{
			ObjMapNode omn;
			memcpy(&omn, &fileData[nodeOffset + sizeof(Node)], sizeof(ObjMapNode));
			
			objects.push_back(omn);
			break;
		}
		
		case NODE_TYPE_COLLISION:
		{
			CollisionNode cn;
			memcpy(&cn, &fileData[nodeOffset + sizeof(Node)], sizeof(CollisionNode));
			
			uint8_t* str = &fileData[cn.offset.offset + sizeof(DataHeader)];
			cout << "Collision node name: " << str << endl;
			break;
		}
		
		case NODE_TYPE_OBJDATA:
		{
			ObjDataNode odn;
			memcpy(&odn, &fileData[nodeOffset + sizeof(Node)], sizeof(ObjDataNode));
			
			uint8_t* str = &fileData[odn.dataNameOffset.offset + sizeof(DataHeader)];
			cout << "Obj data name: " << str << endl;
			break;
		}
		
		case NODE_TYPE_BONES:
		{
			BoneNode bn;
			memcpy(&bn, &fileData[nodeOffset + sizeof(Node)], sizeof(BoneNode));
			
			bones.push_back(extractBones(fileData, bn));
			break;
		}
		
		case NODE_TYPE_BONE_NAME:
		{
			BoneNameNode bnn;
			memcpy(&bnn, &fileData[nodeOffset + sizeof(Node)], sizeof(BoneNameNode));
			
			boneNames.push_back(extractBoneName(fileData, bnn));
			break;
		}
		
		default:
			break;
		
	}
	
	//Read the children of this node
	for(uint32_t i = 0; i < node.numChildren; i++)	//Base case: no children
	{
		//Read in this offset
		offsetList offset;
		memcpy(&offset, &fileData[node.childListOffset+i*sizeof(offsetList)], sizeof(offsetList));
		
		//Recurse
		readNode(fileData, offset.offset, numTabs+1);
	}
}

void decomp(string filename) 
{
	//Clear any leftover data from old files
	textureFilenames.clear();
	objTextureMap.clear();
	faces.clear();
	vertices.clear();
	objects.clear();
	
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
		cout << "ERROR: Not a WF3D file: " << filename << endl;
		return;
	}
	
	//Read the root node
	readNode(fileData, sizeof(wf3dHeader), 0);
	
	outputObj(filename);
	
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
	//Parse commandline
	for(int i = 1; i < argc; i++)
		decomp(argv[i]);
	
	return 0;
}
