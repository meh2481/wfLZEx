SHELL=C:/Windows/System32/cmd.exe
objects = main.o wfLZ.o
LIBPATH = -L./lib
LIB = -lsquish -lFreeImage
HEADERPATH = -I./include
STATICGCC = -static-libgcc -static-libstdc++

all : wfLZEx.exe
 
wfLZEx.exe : $(objects)
	g++ -Wall -O2 -s -o $@ $(objects) $(LIBPATH) $(LIB) $(STATICGCC) $(HEADERPATH)
	
%.o: %.cpp
	g++ -O2 -c -MMD -s -o $@ $< $(HEADERPATH)

-include $(objects:.o=.d)

.PHONY : clean
clean :
	rm -rf wfLZEx.exe *.o *.d
