They Look Ahead, We Look Back!

1. Build LSQUIC Platform (documentation at https://github.com/litespeedtech/lsquic):
	- copy the repository github at https://github.com/cd-athena/wish-retransmission;
	- INSTALL BORINGSSL, CMake and LSQUIC
	Pre-requisites:
	- sudo apt-get update && sudo apt-get install build-essential
	- sudo apt-get install golang
	- sudo apt install zlib1g-dev
	- sudo apt-get install libevent-dev
	- sudo apt-get install libssl-dev
2. Build and Set-up Gurobi (documentation at https://www.gurobi.com/documentation/9.1/quickstart_linux/index.html):
	- Register and obtain an academic license (https://www.gurobi.com/documentation/9.1/quickstart_linux/creating_a_new_academic_li.html);
	- Download the Gurobi optimizer tool (https://www.gurobi.com/downloads/);
	- Activate Gurobi with the obtained license (https://www.gurobi.com/documentation/9.1/quickstart_linux/retrieving_a_free_academic.html);
	- Set the respective Gurobi environmental variables (!) (https://www.gurobi.com/documentation/9.1/quickstart_linux/software_installation_guid.html);
	- Ready to be used!
3. Make the .c files and run the experiments:
	- Run the "cmake ." command followed by the "make" one right before the "bin/" directory of LSQUIC (This should create the executable files in "bin/");
	BEFORE it: use CMakeList.txt of Daniele
	export GUROBI_HOME="/opt/gurobi901/linux64"
	export PATH="${PATH}:${GUROBI_HOME}/bin"
	export LD_LIBRARY_PATH="${GUROBI_HOME}/lib"
	export LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:${GUROBI_HOME}/lib"


	- Open two linux shells (three if we want to run the "tc" tool or a bandiwdth control script) or connect two real or virtual machine to run the experiment;
	- On both machines change directory to the working directory by running "cd boringssl/lsquic/";
	- On the first server's shell run "sudo ./bin/A_2018_01_26_11_26_26_good_4M.sh" for running the bandwidth control script;
	- On the second one run "./bin/http_server -c www.optimized-abr.com,ca-cert.pem,ca-key.pem -s 0.0.0.0:123456":
		-c requires the domain name and the CA certificate and key;
		-s indicates the ip:port available to open a connection.
	- On the client's shell run "./bin/http_client -H www.optimized-abr.com -s 192.168.76.132:123456 -p sintel/235/segment_1.m4s -M GET -K -r 27 -w 1 -J 6":
		-H requires the hostname (domain name) of the machine to connect to;
		-s indicates the ip:port of the server machine available to open a connection.
		-p indicates the path of the (first) requested element (it should actually be an MPD file to parse the streams information from);
		-M connection method (GET/POST);
		-K discard response (don't show media segments);
		-r maximum number of requests to the server before closing the connection -> (set as the maximum number of available media segments);
		-w maximum number of concurrently opened streams;
		-J selection of ABR algorithm type ("1 -> MaxJ", "2 -> MaxMinJ", "3 -> MaxJQ", "4 -> MaxR", "5 -> BOLA", "6 -> SARA").
