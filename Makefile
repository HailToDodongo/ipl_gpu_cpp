CXXFLAGS += -O3 -march=native -msse4.2 -std=c++2b
LINKFLAGS += -lvulkan /usr/local/lib/libvuh.so

all: ipl_gpu shader

shader: shader/shader.glsl
	glslc -O --target-env=vulkan1.2 \
			  -fshader-stage=compute \
			  shader/shader.glsl -o shader/shader.spv

ipl_gpu: main.cpp
	$(CXX) $(CXXFLAGS) main.cpp -o ipl_gpu.o -c
	$(CXX) $(CXXFLAGS) ipl_gpu.o -o ipl_gpu $(LINKFLAGS)

clean:
	rm -f *.o ipl_gpu
