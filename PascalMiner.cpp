// PascalCoin OpenCL miner adapted from the Siacoin GPU miner developed by NebulousLabs.

#include <stdio.h>
#include <stdlib.h>
#include <CL/cl.h>

#include <ctime>
#include <chrono>
#include <cstdint>
#include <Windows.h>

// 2^intensity hashes are calculated each time the kernel is called
// Minimum of 2^8 (256) because our default local_item_size is 256
// global_item_size (2^intensity) must be a multiple of local_item_size
// Max of 2^32 so that people can't send an hour of work to the GPU at one time
#define MIN_INTENSITY		8
#define MAX_INTENSITY		32
#define DEFAULT_INTENSITY	20

// Number of times the GPU kernel is called between updating the command line text
#define MIN_CPI 		1     // Must do one call per update
#define MAX_CPI 		65536 // 2^16 is a slightly arbitrary max
#define DEFAULT_CPI		900

// The maximum size of the .cl file we read in and compile
#define MAX_SOURCE_SIZE 	(0x200000)

// Objects needed to call the kernel
// global namespace so our grindNonce function can access them
cl_command_queue command_queue = NULL;
cl_kernel kernel = NULL;
cl_int ret;

// mem objects for storing our kernel parameters
cl_mem blockHeadermobj = NULL;
cl_mem nonceOutmobj = NULL;

// More gobal variables the grindNonce needs to access
size_t local_item_size = 192; // Size of local work groups. 256 is usually optimal
unsigned int blocks_mined = 0;
unsigned int intensity = DEFAULT_INTENSITY;
static volatile int quit = 0;

// If we get a corrupt target, we want to remember so that if subsequent curl calls
// reutrn more corrupt targets, we don't spam the cmd line with errors
int target_corrupt_flag = 0;

int deviceToUse = 0;

unsigned char* hexToByteArray(const char* hexstring)
{
	size_t len = 176 * 2;
	size_t final_len = len / 2;
	unsigned char* chrs = (unsigned char*)malloc((final_len)* sizeof(*chrs));
	for (size_t i = 0, j = 0; j<final_len; i += 2, j++)
		chrs[j] = (hexstring[i] % 32 + 9) % 25 * 16 + (hexstring[i + 1] % 32 + 9) % 25;
	return chrs;
}

#define headerSize 176

// Only used for determining hashrate, and it's this method's fault that the hashrate sometimes shows as negative (this "rolls over" since nothing over the hour is used in creating the relative time)
long getTimeMillis()
{
	SYSTEMTIME st;
	GetSystemTime(&st);
	return st.wDay * 24 * 60 * 60 * 1000 + st.wHour * 60 * 60 * 1000 + st.wMinute * 60 * 1000 + st.wSecond * 1000 + st.wMilliseconds;
}

char hex[176 * 2 + 1];
int increment = 0;
int callInc = 0;
char old[4] = { 0x00, 0x00, 0x00, 0x00 }; // Used for detecting block hashing info changes


bool different(char* one, char* two, int length)
{
	int i = 0;
	for (; i < length; i++)
	{
		if (one[i] != two[i]) return true;
	}
	return false;
}

static const uint32_t k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
	0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
	0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
	0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
	0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
	0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROTRIGHT(a,b) ((a >> b) | (a << (32 - b)))
#define SIG0(x) (ROTRIGHT(x,7) ^ ROTRIGHT(x,18) ^ ((x) >> 3))
#define SIG1(x) (ROTRIGHT(x,17) ^ ROTRIGHT(x,19) ^ ((x) >> 10))


void getMidstate(uint32_t *midstate, uint32_t *remainingHeader)
{
	if (callInc % 1 == 0)
	{
		callInc = 0;
		FILE *fr;


		char fileName[16] = "headeroutXX.txt";
		fileName[9] = (deviceToUse / 10) + 48;
		fileName[10] = (deviceToUse % 10) + 48;


		fr = fopen(fileName, "rt");
		fgets(hex, 353, fr);
		hex[352] = '\0';

		if (different(old, hex, 4))
		{
			old[0] = hex[0];
			old[1] = hex[1];
			old[2] = hex[2];
			old[3] = hex[3];

			printf(" Real: %s\n", hex);
		}

		fclose(fr);
	}

	callInc++;
	unsigned char* bufferHeaderAux = hexToByteArray(hex);

	unsigned char* bufferHeader = (unsigned char*)malloc((176)* sizeof(char));

	memcpy(bufferHeader, bufferHeaderAux, 176);

	increment++;
	long time = std::time(0);

	bufferHeader[168] = (time & 0x000000FF);
	bufferHeader[169] = (time & 0x0000FF00) >> 8;
	bufferHeader[170] = (time & 0x00FF0000) >> 16;
	bufferHeader[171] = (time & 0xFF000000) >> 24;

	for (int j = 32; j < 48; ++j)
	{
		remainingHeader[j - 32] = (bufferHeader[j * 4 + 0] << 24) | (bufferHeader[j * 4 + 1] << 16) | (bufferHeader[j * 4 + 2] << 8) | (bufferHeader[j * 4 + 3]);
	}

	uint32_t block[64];

	uint32_t h0 = 0x6a09e667;
	uint32_t h1 = 0xbb67ae85;
	uint32_t h2 = 0x3c6ef372;
	uint32_t h3 = 0xa54ff53a;
	uint32_t h4 = 0x510e527f;
	uint32_t h5 = 0x9b05688c;
	uint32_t h6 = 0x1f83d9ab;
	uint32_t h7 = 0x5be0cd19;

	uint32_t a = h0;
	uint32_t b = h1;
	uint32_t c = h2;
	uint32_t d = h3;
	uint32_t e = h4;
	uint32_t f = h5;
	uint32_t g = h6;
	uint32_t h = h7;

	/* 16 * 32 = 512 bits, the size of a chunk in SHA-256 */
	for (int i = 0; i < 16; i++)
	{
		block[i] = ((uint32_t)bufferHeader[i * 4 + 0] << 24) | ((uint32_t)bufferHeader[i * 4 + 1] << 16) | ((uint32_t)bufferHeader[i * 4 + 2] << 8) | ((uint32_t)bufferHeader[i * 4 + 3]);
	}

	for (int i = 16; i < 64; i++)
	{
		block[i] = block[i - 16] + block[i - 7] + SIG1(block[i - 2]) + SIG0(block[i - 15]);
	}

	uint32_t temp1;
	uint32_t temp2;
	uint32_t S1;
	uint32_t S0;

	for (int i = 0; i < 64; i++)
	{
		S1 = (ROTRIGHT(e, 6)) ^ (ROTRIGHT(e, 11)) ^ (ROTRIGHT(e, 25));
		temp1 = h + S1 + ((e & f) ^ ((~e) & g)) + k[i] + block[i];
		S0 = (ROTRIGHT(a, 2)) ^ (ROTRIGHT(a, 13)) ^ (ROTRIGHT(a, 22));
		temp2 = S0 + (((a & b) ^ (a & c) ^ (b & c)));

		h = g;
		g = f;
		f = e;
		e = d + temp1;
		d = c;
		c = b;
		b = a;
		a = temp1 + temp2;
	}

	h0 += a;
	h1 += b;
	h2 += c;
	h3 += d;
	h4 += e;
	h5 += f;
	h6 += g;
	h7 += h;

	/* Now we do most of it again for the 2nd expansion/compression of the 2nd block (bits 513 to 1024)... */

	a = h0;
	b = h1;
	c = h2;
	d = h3;
	e = h4;
	f = h5;
	g = h6;
	h = h7;

	/* 16 * 32 = 512 bits, the size of a chunk in SHA-256 */

	for (int i = 0; i < 16; i++)
	{
		block[i] = ((uint32_t)bufferHeader[(i + 16) * 4 + 0] << 24) | ((uint32_t)bufferHeader[(i + 16) * 4 + 1] << 16) | ((uint32_t)bufferHeader[(i + 16) * 4 + 2] << 8) | ((uint32_t)bufferHeader[(i + 16) * 4 + 3]);
	}

	free(bufferHeader);
	free(bufferHeaderAux);

	for (int i = 16; i < 64; i++)
	{
		block[i] = block[i - 16] + block[i - 7] + SIG1(block[i - 2]) + SIG0(block[i - 15]);
	}

	for (int i = 0; i < 64; i++)
	{
		S1 = (ROTRIGHT(e, 6)) ^ (ROTRIGHT(e, 11)) ^ (ROTRIGHT(e, 25));
		temp1 = h + S1 + ((e & f) ^ ((~e) & g)) + k[i] + block[i];
		S0 = (ROTRIGHT(a, 2)) ^ (ROTRIGHT(a, 13)) ^ (ROTRIGHT(a, 22));
		temp2 = S0 + (((a & b) ^ (a & c) ^ (b & c)));

		h = g;
		g = f;
		f = e;
		e = d + temp1;
		d = c;
		c = b;
		b = a;
		a = temp1 + temp2;
	}

	h0 += a;
	h1 += b;
	h2 += c;
	h3 += d;
	h4 += e;
	h5 += f;
	h6 += g;
	h7 += h;

	midstate[0] = h0;
	midstate[1] = h1;
	midstate[2] = h2;
	midstate[3] = h3;
	midstate[4] = h4;
	midstate[5] = h5;
	midstate[6] = h6;
	midstate[7] = h7;
}



long start = getTimeMillis();

uint32_t totalNonces = 0;

uint32_t callCount = 0;

double grindNonces(int cycles_per_iter)
{
	// Start timing this iteration.

	uint32_t *midState = (uint32_t*)malloc((8)* sizeof(uint32_t));
	uint32_t *remainingHeader = (uint32_t*)malloc((13)* sizeof(uint32_t));


	char nonceOut[8] = { 0 };

	// Get new block header and target.
	getMidstate(midState, remainingHeader);

	int i;

	size_t global_item_size = 0x01 << intensity;

	uint32_t allData[25];
	allData[0] = remainingHeader[0];
	allData[1] = remainingHeader[1];
	allData[2] = remainingHeader[2];
	allData[3] = remainingHeader[3];
	allData[4] = remainingHeader[4];
	allData[5] = remainingHeader[5];
	allData[6] = remainingHeader[6];
	allData[7] = remainingHeader[7];
	allData[8] = remainingHeader[8];
	allData[9] = remainingHeader[9];
	allData[10] = remainingHeader[10];
	allData[11] = remainingHeader[11];

	allData[12] = 0x80000000;
	allData[13] = 0x00000000;
	allData[14] = 0x00000000;
	allData[15] = 0x00000580;
	allData[16] = midState[0];
	allData[17] = midState[1];
	allData[18] = midState[2];
	allData[19] = midState[3];
	allData[20] = midState[4];
	allData[21] = midState[5];
	allData[22] = midState[6];
	allData[23] = midState[7];

	for (i = 0; i < cycles_per_iter * 16; i++) 
	{
		callCount++;
		if (callCount >= 256) 
		{
			callCount = 0;
		}
		allData[24] = callCount;
		// Offset global ids so that each loop call tries a different set of
		// hashes.
		size_t globalid_offset = i * global_item_size;

		// Copy input data to the memory buffer.
		ret = clEnqueueWriteBuffer(command_queue, blockHeadermobj, CL_TRUE, 0, 25 * sizeof(uint32_t), allData, 0, NULL, NULL);
		if (ret != CL_SUCCESS) 
		{
			printf("failed to write to blockHeadermobj buffer: %d\n", ret); exit(1);
		}
		ret = clEnqueueWriteBuffer(command_queue, nonceOutmobj, CL_TRUE, 0, 8 * sizeof(uint8_t), nonceOut, 0, NULL, NULL);
		if (ret != CL_SUCCESS)
		{
			printf("failed to write to targmobj buffer: %d\n", ret); exit(1);
		}

		// Run the kernel.
		ret = clEnqueueNDRangeKernel(command_queue, kernel, 1, &globalid_offset, &global_item_size, &local_item_size, 0, NULL, NULL);
		if (ret != CL_SUCCESS) 
		{
			printf("failed to start kernel: %d\n", ret); exit(1);
		}

		// Copy result to host and see if a block was found.
		ret = clEnqueueReadBuffer(command_queue, nonceOutmobj, CL_TRUE, 0, 8 * sizeof(uint8_t), nonceOut, 0, NULL, NULL);
		if (ret != CL_SUCCESS) 
		{
			printf("failed to read nonce from buffer: %d\n", ret); exit(1);
		}

		if (nonceOut[0] != 0)
		{
			uint32_t nonce = (uint32_t)nonceOut[3] & 0x000000FF | (((uint32_t)nonceOut[2] & 0x000000FF) << 8) | (((uint32_t)nonceOut[1] & 0x000000FF) << 16) | (((uint32_t)nonceOut[0] & 0x000000FF) << 24);
			uint32_t timestamp = remainingHeader[10];
				
			timestamp = ((timestamp & 0x000000FF) << 24) + ((timestamp & 0x0000FF00) << 8) + ((timestamp & 0x00FF0000) >> 8) + ((timestamp & 0xFF000000) >> 24);
			printf("Found nonce: %08x    T: %08x    Hashrate: %.3f MH/s   Total: %d\n", nonce, timestamp, (((((double)totalNonces) * 4 * 16 * 16 * 16 * 16) / (4)) / (((double)getTimeMillis() - start) / 1000)), totalNonces);
			
			
			FILE* f2; 

			char fileName[13] = "datainXX.txt";
			fileName[6] = (deviceToUse / 10) + 48;
			fileName[7] = (deviceToUse % 10) + 48;

			f2 = fopen(fileName, "w");
			while (f2 == NULL)
			{
				f2 = fopen(fileName, "w");
			}

			fprintf(f2, "\$%08x\n", nonce);
			fprintf(f2, "\$%08x", timestamp);
			fclose(f2);  

			


			*nonceOut = 0;
			totalNonces++;
		}
	}

// Not really used--left for potential future use
#ifdef __linux__
	clock_gettime(CLOCK_REALTIME, &end);
	double nsElapsed = 1e9 * (double)(end.tv_sec - begin.tv_sec) + (double)(end.tv_nsec - begin.tv_nsec);
	double run_time_seconds = nsElapsed * 1e-9;
#else
	double run_time_seconds = (double)(clock() - 500) / CLOCKS_PER_SEC;
#endif

	// Calculate the hash rate of thie iteration.
	double hash_rate = cycles_per_iter * global_item_size / (run_time_seconds * 1000000);
	return hash_rate;
}
 
void selectOCLDevice(cl_platform_id *OCLPlatform, cl_device_id *OCLDevice, cl_uint platformid, cl_uint deviceidx) 
{
	cl_uint platformCount, deviceCount;
	cl_platform_id *platformids;
	cl_device_id *deviceids;
	cl_int ret;

	ret = clGetPlatformIDs(0, NULL, &platformCount);
	if (ret != CL_SUCCESS)
	{
		printf("Failed to get number of OpenCL platforms with error code %d (clGetPlatformIDs).\n", ret);
		exit(1);
	}

	// If we don't exit here, the default platform ID chosen MUST be valid; it's zero.
	// I return 0, because this isn't an error - there is simply nothing to do.
	if (!platformCount)
	{
		printf("OpenCL is reporting no platforms available on the system. Nothing to do.\n");
		exit(0);
	}

	// Since the number of platforms returned is the number of indexes plus one,
	// the default platform ID (zero), must exist. User may still specify something
	// invalid, however, so check it.
	if (platformCount <= platformid)
	{
		printf("Platform selected (%u) is the same as, or higher than, the number ", platformid);
		printf("of platforms reported to exist by OpenCL on this system (%u). ", platformCount);
		printf("Remember that the first platform has index 0!\n");
		exit(1);
	}

	platformids = (cl_platform_id *)malloc(sizeof(cl_platform_id) * platformCount);

	ret = clGetPlatformIDs(platformCount, platformids, NULL);
	if (ret != CL_SUCCESS) 
	{
		printf("Failed to retrieve OpenCL platform IDs with error code %d (clGetPlatformIDs).\n", ret);
		exit(1);
	}

	// Now fetch device ID list for this platform similarly to the fetch for the platform IDs.
	// platformid has been verified to be within bounds.
	ret = clGetDeviceIDs(platformids[platformid], CL_DEVICE_TYPE_GPU, 0, NULL, &deviceCount);
	if (ret != CL_SUCCESS) 
	{
		printf("Failed to get number of OpenCL devices with error code %d (clGetDeviceIDs).\n", ret);
		free(platformids);
		exit(1);
	}

	// If we have no devices, indicate this to the user.
	if (!deviceCount) 
	{
		printf("OpenCL is reporting no GPU devices available for chosen platform. Nothing to do.\n");
		free(platformids);
		exit(0);
	}

	// Check that the device we've been asked to get does, in fact, exist...
	if (deviceCount <= deviceidx)
	{
		printf("Device selected (%u) is the same as, or higher than, the number ", deviceidx);
		printf("of GPU devices reported to exist by OpenCL on the current platform (%u). ", deviceCount);
		printf("Remember that the first device has index 0!\n");
		free(platformids);
		exit(1);
	}

	deviceids = (cl_device_id *)malloc(sizeof(cl_device_id) * deviceCount);

	ret = clGetDeviceIDs(platformids[platformid], CL_DEVICE_TYPE_GPU, deviceCount, deviceids, NULL);
	if (ret != CL_SUCCESS) 
	{
		printf("Failed to retrieve OpenCL device IDs for selected platform with error code %d (clGetDeviceIDs).\n", ret);
		free(platformids);
		free(deviceids);
		exit(1);
	}

	// Done. Return the platform ID and device ID object desired, free lists, and return.
	*OCLPlatform = platformids[platformid];
	*OCLDevice = deviceids[deviceidx];
}

void printPlatformsAndDevices() 
{
	cl_uint platformCount, deviceCount;
	cl_platform_id *platformids;
	cl_device_id *deviceids;
	cl_int ret;

	ret = clGetPlatformIDs(0, NULL, &platformCount);
	if (ret != CL_SUCCESS || !platformCount)
	{
		printf("Could not find any opencl platforms on your computer.\n");
		return;
	}
	printf("Found %u platform(s) on your computer.\n", platformCount);

	platformids = (cl_platform_id *)malloc(sizeof(cl_platform_id) * platformCount);

	ret = clGetPlatformIDs(platformCount, platformids, NULL);
	if (ret != CL_SUCCESS) 
	{
		printf("Error while fetching platform ids.\n");
		free(platformids);
		return;
	}

	int i, j; // Iterate through each platform and print its devices

	for (i = 0; i < platformCount; i++)
	{
		char str[80];
		// Print platform info.
		ret = clGetPlatformInfo(platformids[i], CL_PLATFORM_NAME, 80, str, NULL);
		if (ret != CL_SUCCESS)
		{
			printf("\tError while fetching platform info.\n");
			continue;
		}
		printf("Devices on platform %d, \"%s\":\n", i, str);
		ret = clGetDeviceIDs(platformids[i], CL_DEVICE_TYPE_GPU, 0, NULL, &deviceCount);
		if (ret != CL_SUCCESS)
		{
			printf("\tError while fetching device ids.\n");
			continue;
		}
		if (!deviceCount) 
		{
			printf("\tNo devices found for this platform.\n");
			continue;
		}
		deviceids = (cl_device_id *)malloc(sizeof(cl_device_id) * deviceCount);

		ret = clGetDeviceIDs(platformids[i], CL_DEVICE_TYPE_GPU, deviceCount, deviceids, NULL);
		if (ret != CL_SUCCESS) 
		{
			printf("\tError while getting device ids.\n");
			free(deviceids);
			continue;
		}

		for (j = 0; j < deviceCount; j++) 
		{
			// Print platform info.
			ret = clGetDeviceInfo(deviceids[j], CL_DEVICE_NAME, 80, str, NULL);
			if (ret != CL_SUCCESS) {
				printf("\tError while getting device info.\n");
				free(deviceids);
				continue;
			}
			printf("\tDevice %d: %s\n", j, str);
		}
		free(deviceids);
	}
	free(platformids);
}

int main(int argc, char* argv[])
{
	printPlatformsAndDevices();
	cl_platform_id platform_id = NULL;
	cl_device_id device_id = NULL;
	cl_context context = NULL;
	cl_program program = NULL;
	cl_uint platformid = 0, deviceidx = 0;
	deviceToUse = deviceidx;

	unsigned cycles_per_iter = 30;

	int i = 0; int j = 0;

	if (argc > 1)
	{
		for (i = 1; i < argc; i++)
		{
			char* argument = argv[i];
			if (argument[0] == 'd')
			{
				deviceidx = deviceToUse = atoi(1 + argument);
			}
			else if (argument[0] == 'i')
			{
				intensity = atoi(1 + argument);
			}
			else if (argument[0] == 'p')
			{
				platformid = atoi(1 + argument);
			}
			else if (argument[0] == 'c')
			{
				cycles_per_iter = atoi(1 + argument);
			}
		}
	}

	double hash_rate;

	FILE *fp;
	const char fileName[] = "./pascalsha.cl";
	size_t source_size;
	char *source_str;

	fp = fopen(fileName, "r");

	if (!fp) 
	{
		fprintf(stderr, "Failed to load kernel.\n");
		return 1;
	}
	source_str = (char *)malloc(0x200000);
	source_size = fread(source_str, 1, 0x200000, fp);
	fclose(fp);

	selectOCLDevice(&platform_id, &device_id, platformid, deviceidx);
	// Make sure the device can handle our local item size.
	size_t max_group_size = 0;
	ret = clGetDeviceInfo(device_id, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof(size_t), &max_group_size, NULL);
	if (ret != CL_SUCCESS)
	{ 
		printf("failed to get Device IDs: %d\n", ret); exit(1);
	}

	if (local_item_size > max_group_size)
	{
		printf("Selected device cannot handle work groups larger than %zu.\n", local_item_size);
		printf("Using work groups of size %zu instead.\n", max_group_size);
		local_item_size = max_group_size;
	}
	// Create OpenCL Context.
	context = clCreateContext(NULL, 1, &device_id, NULL, NULL, &ret);

	// Create command queue.
	command_queue = clCreateCommandQueue(context, device_id, 0, &ret);

	// Create Buffer Objects.
	blockHeadermobj = clCreateBuffer(context, CL_MEM_READ_ONLY, 25 * sizeof(uint32_t), NULL, &ret);
	if (ret != CL_SUCCESS) 
	{ 
		printf("failed to create blockHeadermobj buffer: %d\n", ret); exit(1); 
	}

	nonceOutmobj = clCreateBuffer(context, CL_MEM_READ_WRITE, 8 * sizeof(char), NULL, &ret);
	if (ret != CL_SUCCESS) 
	{
		printf("failed to create nonceOutmobj buffer: %d\n", ret); exit(1);
	}

	// Create kernel program from source file.
	program = clCreateProgramWithSource(context, 1, (const char **)&source_str, (const size_t *)&source_size, &ret);
	if (ret != CL_SUCCESS) 
	{ 
		printf("failed to crate program with source: %d\n", ret); exit(1);
	}

	ret = clBuildProgram(program, 1, &device_id, NULL, NULL, NULL);
	if (ret != CL_SUCCESS) 
	{
		// Print information about why the build failed. This code is from
		// StackOverflow.
		size_t len;
		char buffer[204800];
		cl_build_status bldstatus;
		printf("\nError %d: Failed to build program executable [ ]\n", ret);
		ret = clGetProgramBuildInfo(program, device_id, CL_PROGRAM_BUILD_STATUS, sizeof(bldstatus), (void *)&bldstatus, &len);
		if (ret != CL_SUCCESS) 
		{
			printf("Build Status error %d\n", ret);
			exit(1);
		}
		if (bldstatus == CL_BUILD_SUCCESS) printf("Build Status: CL_BUILD_SUCCESS\n");
		if (bldstatus == CL_BUILD_NONE) printf("Build Status: CL_BUILD_NONE\n");
		if (bldstatus == CL_BUILD_ERROR) printf("Build Status: CL_BUILD_ERROR\n");
		if (bldstatus == CL_BUILD_IN_PROGRESS) printf("Build Status: CL_BUILD_IN_PROGRESS\n");
		ret = clGetProgramBuildInfo(program, device_id, CL_PROGRAM_BUILD_OPTIONS, sizeof(buffer), buffer, &len);
		if (ret != CL_SUCCESS) 
		{
			printf("Build Options error %d\n", ret);
			exit(1);
		}
		printf("Build Options: %s\n", buffer);
		ret = clGetProgramBuildInfo(program, device_id, CL_PROGRAM_BUILD_LOG, sizeof(buffer), buffer, &len);
		if (ret != CL_SUCCESS) 
		{
			printf("Build Log error %d\n", ret);
			exit(1);
		}
		printf("Build Log:\n%s\n", buffer);
		exit(1);
	}

	// Create data parallel OpenCL kernel.
	kernel = clCreateKernel(program, "nonceGrind", &ret);

	// Set OpenCL kernel arguments.
	void *args[] = { &blockHeadermobj, &nonceOutmobj };
	for (i = 0; i < 2; i++) 
	{
		ret = clSetKernelArg(kernel, i, sizeof(cl_mem), args[i]);
		if (ret != CL_SUCCESS)
		{
			printf("failed to set kernel arg %d (error code %d)\n", i, ret);
			exit(1);
		}
	}
	printf("\n");

	// signal(SIGINT, quitSignal);
	while (!quit) 
	{
		// Repeat until no block is found.
		do 
		{
			hash_rate = grindNonces(cycles_per_iter);
		} while (hash_rate == -1 && !quit);

		if (!quit) 
		{
			// printf("\rMining at %.3f MH/s\t%u blocks mined", hash_rate, blocks_mined);
			fflush(stdout);
		}
	}
}

