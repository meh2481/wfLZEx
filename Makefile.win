SHELL=C:/Windows/System32/cmd.exe
objects = main.o wfLZ.o
o3d = wf3dEx.o wfLZ.o
LIBPATH = -L./lib
LIB = -lsquish -lFreeImage
HEADERPATH = -I./include
STATICGCC = -static-libgcc -static-libstdc++

all : wfLZEx.exe wf3dEx.exe
 
wfLZEx.exe : $(objects)
	g++ -Wall -O2 -s -o $@ $(objects) $(LIBPATH) $(LIB) $(STATICGCC) $(HEADERPATH)

wf3dEx.exe : $(o3d)
	g++ -Wall -O2 -s -o $@ $(o3d) $(LIBPATH) $(LIB) $(STATICGCC) $(HEADERPATH)
	
%.o: %.cpp
	g++ -O2 -c -MMD -s -o $@ $< $(HEADERPATH)

-include $(objects:.o=.d)

.PHONY : clean
clean :
	rm -rf wfLZEx.exe *.o *.d
