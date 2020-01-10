// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/FalseColor.h>

using namespace std;

TEV_NAMESPACE_BEGIN

#define RGB(r,g,b) r / 255.f, g / 255.f, b / 255.f, 1.f
const vector<float>& falseColorData() {
    static const vector<float> falseColorData = {
        RGB(  0,   0,   0),
        RGB( 80,  18, 123),
        RGB(181,  54, 121),
        RGB(251, 136,  97),
        RGB(251, 252, 191)
    };

    return falseColorData;
}
#undef RGB

TEV_NAMESPACE_END
