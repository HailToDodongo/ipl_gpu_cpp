# IPL-Checksum GPU Hasher

N64 IPL Hash Brute-forcer using Vulkan + Compute Shaders.<br>
> **Note:**<br>
> This assumes the `CIC 6105/7105` with the checksum `8618 A45B C2D3`.<br>
> You should set the seed to `9191` when using this tool.

## Usage
Once compiled run:
```bash
./ipl_gpu nonMatchingROM.z64 9191
```
If a collision is found, the program will exit and dump the patched ROM to `match.z64`.<br>

## Building

Run `make all` to build the project incl. shaders.<br>

Building shaders requires `glslc` from the `Vulkan SDK`.<br>
A compiled `.spv` shader is checked in too.<br>

On the C++ side, this project requires: [VUH](https://github.com/Glavnokoman/vuh).<br>
If you run into trouble compiling it (i did with clang), try the following patch:

```diff
diff --git a/src/device.cpp b/src/device.cpp
index c767f2e..e78f162 100644
--- a/src/device.cpp
+++ b/src/device.cpp
@@ -218,7 +218,7 @@ namespace vuh {
        {
                auto pipelineCI = vk::ComputePipelineCreateInfo(flags
                                                                                                                                                , shader_stage_info, pipe_layout);
-               return createComputePipeline(pipe_cache, pipelineCI, nullptr);
+               return createComputePipeline(pipe_cache, pipelineCI, nullptr).value;
                
        }
 
diff --git a/src/include/vuh/delayed.hpp b/src/include/vuh/delayed.hpp
index 88141ba..48d5f5f 100644
--- a/src/include/vuh/delayed.hpp
+++ b/src/include/vuh/delayed.hpp
@@ -5,6 +5,7 @@
 #include <vuh/resource.hpp>
 
 #include <cassert>
+#include <memory> 
 
 namespace vuh {
        namespace detail{
diff --git a/src/include/vuh/utils.h b/src/include/vuh/utils.h
index 2c2c089..16f1396 100644
--- a/src/include/vuh/utils.h
+++ b/src/include/vuh/utils.h
@@ -2,6 +2,7 @@
 
 #include <stdint.h>
 #include <vector>
+#include <cstdint>
 
 namespace vuh {
```

## License
This project is licensed under the MIT License - see [LICENSE](LICENSE)<br>
© 2023 - Max Bebök (HailToDodongo)