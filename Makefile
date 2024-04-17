all:np_multi_proc.cpp 
	g++ -pthread -o np_multi_proc np_multi_proc.cpp
clean:
	$(RM) np_multi_proc