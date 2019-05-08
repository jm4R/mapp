# miniaudio++

The high-level C++11 bindings for the **miniaudio** C library. See the [miniaudio project on github](https://github.com/dr-soft/miniaudio) for details.

Cloning and building
========
This is headers only library that requires C++11. So just include `mapp.hpp` to your code. 

Please als note that this repository is dependent on miniaudio git submodule, so clone recursive, e.g.:

    git clone --recursive https://github.com/jm4R/mapp.git

Simple Playback Example
========

```c++
#include <mapp.hpp>
#include <iostream>

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cerr << "No input file(s)" << std::endl;
        return -1;
    }

    try {
        mapp::oastream caout;
        for (int i=1; i<argc; ++i)
        {
             mapp::audio_file af{ argv[i] };
             caout << af; //async call
             af.wait();
        }
    } catch (mapp::mapp_exception& ex) {
        std::cerr << "Audio error: " << ex.what() << std::endl;
        return -1;
    }

    return 0;
}

```

Fast documentation
========

* `oastream` – represents audio device. Most probably you should use only one instance per whole application. Call `stop` on it when idle for a long time.

* `audio_file` - audio played directly from the file.

* `audio_memory_view` - audio played directly from the memory. Does not take ownership of the memory.