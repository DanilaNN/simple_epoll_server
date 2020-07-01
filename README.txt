Simple epoll server written in c. Tested on Ubuntu 16.04 LTS.
Tests were done with curl and ab (apache benchmark)
http_parser is third party code.

1. How to build:
mkdir build
cd build
cmake ..
make
2. How to run:
cd build
./server -h <ip> -p <port> -d <directory>
Example: "./server -h 127.0.0.10 -p 12345 -d /tmp"
3. Available requests:
GET
4. Test examples:
curl -I -0 -X GET http://127.0.0.1:12345/index.html
ab -n 1000 -c 100 http://127.0.0.1:12345/index.html (sends 1000 requests with 100 concurrent requests)	
5. Known issues:
Sometimes server do not accept connections after start, need to rerun it.
