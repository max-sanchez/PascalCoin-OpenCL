To use this miner:

1. You must already have PascalCoin installed. If you don't have it, download it from sourceforge here: https://sourceforge.net/projects/pascalcoin/. Once it is installed, run the PascalCoinWallet.exe provided in the download.

2. You must be using a 256-bit secp256k1 key. This is the default behavior of the PascalCoin wallet.

3. Your miner name must be exactly 8 characters long. The miner expects that the input is exactly 176 total bytes (which is achieved by using a secp256k1 key and a 10-character name)  NOTE: NOT 10 like before! 8 characters, because the last two will be used to identify each GPU!

4. You must have RPC enabled in your client (any port of your choosing, default is 4009)

5. You must run the proxy miner (PascalProxyv2.jar) in the same directory as the PascalCoinCUDA_ProxyMiner_smXX.exe file you run (everything is where it needs to be if you just extract the provided zip). For most people, the host should be 127.0.0.1, and the port should be 4009. Enter the same 8-character miner name you put in your PascalCoin wallet.

6. Open one or more (one for each GPU) copy of PascalCoinOpenCL_ProxyMiner.exe by cd-ing to this directory in a command prompt, and running PascalCoinOpenCL_ProxyMiner.exe. 
The miner takes four arguments: device (d), platform (p), intensity (i), and cyclesize (c). 

For example: PascalCoinOpenCL_ProxyMiner.exe d0 p0 i23 c50
would run the miner on platform 0, device 0, with an intensity of 23 and a cyclesize of 50. 

Higher intensities are more demanding on the GPU. Additionally, too high of an intensity can cause the miner to actually decrease in effective hashrate. 

The cyclesize has a minimal affect on hashrate generally. 

Unless you have a special setup (like NVidia and AMD cards in the same system), your platform is probably 0. You can determine which platform and device to use from the output at the original miner's start.

You can also run the benchmark miner (in the Benchmark folder) to try out different devices and intensity/cyclesize arguments. If you are compiling from source and want to make the benchmark version instead of the normal mining version, comment out the lines responsible for writing to the datainXX.txt file, (optionally) change the header file you read from to a single name regardless of device name, change the line

printf("Found nonce: %08x    T: %08x    Hashrate: %.3f MH/s   Total: %d\n", nonce, timestamp, (((((double)totalNonces) * 4 * 16 * 16 * 16 * 16) / (4)) / (((double)getTimeMillis() - start) / 1000)), totalNonces);

to

printf("Found nonce: %08x    T: %08x    Hashrate: %.3f MH/s   Total: %d\n", nonce, timestamp, (((((double)totalNonces) * 4 * 16 * 8) / (4)) / (((double)getTimeMillis() - start) / 1000)), totalNonces);
			
			
 and modify the .cl code by changing the lines:

		uint targetX = h0 & 0xFFFFFFFF;
		uint targetY = h1 & 0xF0000000;
to
		uint targetX = h0 & 0xFFFFFF70;
		uint targetY = h1 & 0x00000000; 

This essentially lets the code find nonces 512 times faster, and accounts for the 512-times-faster-sharerate by reducing how many hashes it expects each nonce solve to take by a factor of 512, while removing the overhead of writing files (since writing a few files a second may cause it to be slower).

If you notice your miner finding several of the same nonce, try lowering the intensity and/or cyclesize (because you're sending so much work to the GPU that it can't get a timestamp often enough, so it exhausts the 4-ish billion possible nonces (~4 GH), and starte repeating work). 
