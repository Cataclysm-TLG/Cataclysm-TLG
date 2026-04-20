// #include "get_version.h" // IWYU pragma: associated
#include <fstream>
#include <string>

//#if (defined(_WIN32) || defined(MINGW)) && !defined(GIT_VERSION) && !defined(CROSS_LINUX) && !defined(_MSC_VER)

//#ifndef VERSION
// #define VERSION "1.0"
//#endif

// Forward declarations (satisfy -Wmissing-declarations)
std::string getVersionFromFile();
const char *getVersionString();

std::string getVersionFromFile()
{
    std::ifstream versionFile( "VERSION.txt" );
    if( versionFile.is_open() ) {
        std::string line;
        while( std::getline( versionFile, line ) ) {
            if( line.rfind( "build number:", 0 ) == 0 ) {
                size_t colonPos = line.find( ':' );
                if( colonPos != std::string::npos ) {
                    std::string buildNumber = line.substr( colonPos + 1 );
                    buildNumber.erase( 0, buildNumber.find_first_not_of( " \t" ) );
                    versionFile.close();
                    return buildNumber;
                }
            }
        }
        versionFile.close();
    }
    return "unknown";
}

const char *getVersionString()
{
    static std::string version = getVersionFromFile();
    return version.c_str();
}