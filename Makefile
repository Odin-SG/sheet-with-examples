all: main.cpp
	g++ -pthread -std=c++11 -g3 main.cpp -o main
