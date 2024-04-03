all:np_simple.cpp 
	g++ np_simple.cpp -o np_simple
clean:
	$(RM) np_simple