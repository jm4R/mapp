#include <iostream>
#include <mapp.hpp>

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cerr << "No input file(s)" << std::endl;
        return -1;
    }

    try
    {
        mapp::oastream caout;
        for (int i = 1; i < argc; ++i)
        {
            mapp::audio_file af{argv[i]};
            af.set_finish_callback(
                [] { std::cout << "finished playing audio" << std::endl; });
            caout << af; // async call
            af.wait();
        }
    }
    catch (mapp::mapp_exception& ex)
    {
        std::cerr << "Audio error: " << ex.what() << std::endl;
        return -1;
    }

    return 0;
}
