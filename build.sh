#!/bin/bash
g++ -std=c++14 -pthread -D ASIO_STANDALONE -D ASIO_HAS_STD_CHRONO -I asio/asio/include main.cpp -o p2p.bin
