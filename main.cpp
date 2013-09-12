#include "wfLZ.h"
#include <iostream>
#include <vector>
#include <string>
#include <stdlib.h>
#include <cstdio>
using namespace std;

int chunkDecompress()
{
	vector<uint8_t> vData;
	while(!cin.eof())
	{
		int val = cin.get();
		if(val != EOF)
			vData.push_back(val);
	}
	
	cerr<< "Got data" << endl;
	
	uint32_t* chunk = NULL;
	const uint32_t decompressedSize = wfLZ_GetDecompressedSize( vData.data() );
	uint8_t* dst = ( uint8_t* )malloc( decompressedSize );
	cerr << "size: " << decompressedSize << endl;
	uint32_t offset = 0;
	int count = 0;
	while( uint8_t* compressedBlock = wfLZ_ChunkDecompressLoop( vData.data(), &chunk ) )
	{
		cerr << "Loop " << count << endl;
		wfLZ_Decompress( compressedBlock, dst + offset );
		cerr << "thing" << endl;
		const uint32_t blockSize = wfLZ_GetDecompressedSize( compressedBlock );
		offset += blockSize;
		cerr << "blocksize: " << blockSize<<endl;
	}
	
	cerr << "Write dest" << endl;
	for(int i = 0; i < decompressedSize; i++)
		cout << dst[i];
	
	free(dst);
	
	return 0;
}

int main(int argc, char** argv)
{
	bool bCompress = false;
	//Compress data; just for testing purposes
	if(argc > 1)
	{
		string s = argv[1];
		if(s == "compress")
			bCompress = true;
		else if(s == "decompress")
			bCompress = false;
		else if(s == "chunk")
			return chunkDecompress();
	}

	if(!bCompress)
	{
		vector<uint8_t> vData;
		while(!cin.eof())
		{
			int val = cin.get();
			if(val != EOF)
				vData.push_back(val);
		}

		uint32_t size = wfLZ_GetDecompressedSize(vData.data());
		cerr<<"Size: " << size << endl;
		uint8_t* out = (uint8_t*) malloc(size);
		cerr <<"pre"<< endl;
		wfLZ_Decompress(vData.data(), out);
		
		cerr <<"post"<< endl;
		for(int i = 0; i < size; i++)
			cout << out[i];

		free(out);
	}

	else
	{
		vector<uint8_t> vData;
		while(!cin.eof())
		{
			int val = cin.get();
			if(val != EOF)
				vData.push_back(val);
		}
		
		uint32_t compressed_bytes = wfLZ_GetMaxCompressedSize(vData.size());

		//Get size of recommended scratchpad.
		uint32_t scratchpad_bytes = wfLZ_GetWorkMemSize();

		uint8_t* compressed = (uint8_t*) malloc(compressed_bytes);

		//Allocate scratchpad buffer.
		uint8_t* scratchpad = (uint8_t*) malloc(scratchpad_bytes);

		//Perform compression.
		uint32_t data_comp_size = wfLZ_CompressFast(vData.data(), vData.size(), compressed, scratchpad, 0);

		//We no longer need scratchpad.
		free(scratchpad);

		for(int i = 0; i < data_comp_size; i++)
		{
			cout << compressed[i];
		}
		
		free(compressed);
	}

	return 0;
}
