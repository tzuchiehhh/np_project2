all:np_single_proc.cpp 
	g++ np_single_proc.cpp -o np_single_proc
clean:
	$(RM) np_single_proc