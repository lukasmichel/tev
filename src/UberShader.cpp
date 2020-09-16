// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/FalseColor.h>
#include <tev/UberShader.h>

using namespace Eigen;
using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

UberShader::UberShader()
: mColorMap{GL_CLAMP_TO_EDGE, GL_LINEAR, false} {
    mShader.define("SRGB",        to_string(ETonemap::SRGB));
    mShader.define("GAMMA",       to_string(ETonemap::Gamma));
    mShader.define("FALSE_COLOR", to_string(ETonemap::FalseColor));
    mShader.define("POS_NEG",     to_string(ETonemap::PositiveNegative));
    mShader.define("COMPLEX",     to_string(ETonemap::Complex));

    mShader.define("ERROR",                   to_string(EMetric::Error));
    mShader.define("ABSOLUTE_ERROR",          to_string(EMetric::AbsoluteError));
    mShader.define("SQUARED_ERROR",           to_string(EMetric::SquaredError));
    mShader.define("RELATIVE_ABSOLUTE_ERROR", to_string(EMetric::RelativeAbsoluteError));
    mShader.define("RELATIVE_SQUARED_ERROR",  to_string(EMetric::RelativeSquaredError));
    mShader.define("DIVISION",                to_string(EMetric::Division));

    mShader.define("IDENTITY",                to_string(EPostProcessing::Identity));
    mShader.define("SQUARE",                  to_string(EPostProcessing::Square));
    mShader.define("CLIP10",                  to_string(EPostProcessing::Clip10));
    mShader.define("CLIP100",                 to_string(EPostProcessing::Clip100));
    mShader.define("MAGNITUDE",               to_string(EPostProcessing::Magnitude));

    mShader.define("M_PI", to_string(M_PI));

    mShader.init(
        "ubershader",

        // Vertex shader
        R"(#version 330

        uniform vec2 pixelSize;
        uniform vec2 checkerSize;

        uniform mat3 imageTransform;
        uniform mat3 referenceTransform;

        in vec2 position;

        out vec2 checkerUv;
        out vec2 imageUv;
        out vec2 referenceUv;

        void main() {
            checkerUv = position / (pixelSize * checkerSize);
            imageUv = (imageTransform * vec3(position, 1.0)).xy;
            referenceUv = (referenceTransform * vec3(position, 1.0)).xy;

            gl_Position = vec4(position, 1.0, 1.0);
        })",

        // Fragment shader
        R"(#version 330

        uniform bool isCropped;
        uniform vec2 cropMin;
        uniform vec2 cropMax;

        uniform sampler2D image;
        uniform bool hasImage;

        uniform sampler2D reference;
        uniform bool hasReference;

        uniform sampler2D colormap;

        uniform float exposure;
        uniform float offset;
        uniform float gamma;
        uniform int tonemap;
        uniform int metric;
        uniform int postProcessing;

        uniform vec4 bgColor;

        in vec2 checkerUv;
        in vec2 imageUv;
        in vec2 referenceUv;

        out vec4 color;

        float average(vec3 col) {
            return (col.r + col.g + col.b) / 3.0;
        }

        vec3 applyExposureAndOffset(vec3 col) {
            return pow(2.0, exposure) * col + offset;
        }

        vec3 applyInverseExposureAndOffset(vec3 col) {
            return pow(2.0, -exposure) * (col - offset);
        }

        vec3 falseColor(float v) {
            //v = log(v) / log(1000000.0);
            v = log2(v+0.03125) / 10.0 + 0.5;
            v = clamp(v, 0.0, 1.0);
            return texture(colormap, vec2(v, 0.5)).rgb;
        }

        float linear(float sRGB) {
            if (sRGB > 1.0) {
                return 1.0;
            } else if (sRGB < 0.0) {
                return 0.0;
            } else if (sRGB <= 0.04045) {
                return sRGB / 12.92;
            } else {
                return pow((sRGB + 0.055) / 1.055, 2.4);
            }
        }

        float sRGB(float linear) {
            if (linear > 1.0) {
                return 1.0;
            } else if (linear < 0.0) {
                return 0.0;
            } else if (linear < 0.0031308) {
                return 12.92 * linear;
            } else {
                return 1.055 * pow(linear, 0.41666) - 0.055;
            }
        }

        vec3 hsl2rgb(vec3 c) {
            vec3 rgb = clamp( abs(mod(c.x*6.0+vec3(0.0,4.0,2.0),6.0)-3.0)-1.0, 0.0, 1.0 );
            return c.z + c.y * (rgb-0.5)*(1.0-abs(2.0*c.z-1.0));
        }

        vec3 applyTonemap(vec3 col) {
            switch (tonemap) {
                case COMPLEX:     return hsl2rgb(vec3(
                    atan(col.y, col.x) / (2.0*M_PI), 1.0, 1.0 - pow(0.5, length(col))
                ));
                case SRGB:        return vec3(sRGB(col.r), sRGB(col.g), sRGB(col.b));
                case GAMMA:       return pow(col, vec3(1.0 / gamma));
                // Here grayscale is compressed such that the darkest color is is 1/1024th as bright as the brightest color.
                case FALSE_COLOR: return falseColor(average(col));
                case POS_NEG:     return vec3(-average(min(col, vec3(0.0))) * 2.0, average(max(col, vec3(0.0))) * 2.0, 0.0);
            }
            return vec3(0.0);
        }

        vec3 applyMetric(vec3 image, vec3 reference) {
            vec3 col = image - reference;
            switch (metric) {
                case ERROR:                   return col;
                case ABSOLUTE_ERROR:          return abs(col);
                case SQUARED_ERROR:           return col * col;
                case RELATIVE_ABSOLUTE_ERROR: return abs(col) / (reference + vec3(0.001));
                case RELATIVE_SQUARED_ERROR:  return col * col / (reference * reference + vec3(0.001));
                case DIVISION:                return (image + vec3(0.001)) / (reference + vec3(0.001));
            }
            return vec3(0.0);
        }

        vec3 applyPostProcessing(vec3 image) {
            switch (postProcessing) {
                case IDENTITY:  return image;
                case SQUARE:    return image * image;
                case CLIP10:    return min(image, 10.f);
                case CLIP100:   return min(image, 100.f);
                case MAGNITUDE: return vec3(length(image));
            }
            return vec3(0.0);
        }

        vec4 sample(sampler2D sampler, vec2 uv) {
            if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
                return vec4(0.0);
            }
            return texture(sampler, uv);
        }

        void main() {
            vec3 darkGray = vec3(0.5, 0.5, 0.5);
            vec3 lightGray = vec3(0.55, 0.55, 0.55);

            vec3 checker = mod(int(floor(checkerUv.x) + floor(checkerUv.y)), 2) == 0 ? darkGray : lightGray;
            checker = bgColor.rgb * bgColor.a + checker * (1.0 - bgColor.a);
            if (!hasImage) {
                color = vec4(checker, 1.0);
                return;
            }

            float cropAlpha = 1.f;
            if (isCropped) {
                if (imageUv.x < cropMin.x
                || imageUv.x > cropMax.x
                || imageUv.y < cropMin.y
                || imageUv.y > cropMax.y)
                    cropAlpha = 0.3f;
            }

            vec4 imageVal = sample(image, imageUv);
            imageVal.a = imageVal.a * cropAlpha;
            if (!hasReference) {
                color = vec4(
                    applyTonemap(
                        applyExposureAndOffset(
                            applyPostProcessing(imageVal.rgb)
                        )
                    ) * imageVal.a +
                    checker * (1.0 - imageVal.a),
                    1.0
                );
                return;
            }

            vec4 referenceVal = sample(reference, referenceUv);
            referenceVal.a = referenceVal.a * cropAlpha;

            float alpha = (imageVal.a + referenceVal.a) * 0.5;
            color = vec4(
                applyTonemap(
                    applyExposureAndOffset(
                        applyMetric(
                            applyPostProcessing(imageVal.rgb),
                            applyPostProcessing(referenceVal.rgb)
                        )
                    )
                ) * alpha +
                checker * (1.0 - alpha),
                1.0
            );
        })"
    );

    // 2 Triangles
    MatrixXu indices(3, 2);
    indices.col(0) << 0, 1, 2;
    indices.col(1) << 2, 3, 0;

    MatrixXf positions(2, 4);
    positions.col(0) << -1, -1;
    positions.col(1) <<  1, -1;
    positions.col(2) <<  1,  1;
    positions.col(3) << -1,  1;

    mShader.bind();
    mShader.uploadIndices(indices);
    mShader.uploadAttrib("position", positions);

    const auto& fcd = colormap::turbo();
    mColorMap.setData(fcd, Vector2i{(int)fcd.size() / 4, 1}, 4);
}

UberShader::~UberShader() {
    mShader.free();
}

void UberShader::draw(const Vector2f& pixelSize, const Vector2f& checkerSize) {
    mShader.bind();
    bindCheckerboardData(pixelSize, checkerSize);
    mShader.setUniform("hasImage", false);
    mShader.setUniform("postProcessing", static_cast<int>(EPostProcessing::Identity));
    mShader.setUniform("hasReference", false);
    mShader.setUniform("isCropped", false);
    mShader.setUniform("cropMin", (Vector2f)Vector2f::Constant(0.f));
    mShader.setUniform("cropMax", (Vector2f)Vector2f::Constant(0.f));
    mShader.drawIndexed(GL_TRIANGLES, 0, 2);
}

void UberShader::draw(
    const Vector2f& pixelSize,
    const Vector2f& checkerSize,
    GlTexture* textureImage,
    const Matrix3f& transformImage,
    float exposure,
    float offset,
    float gamma,
    ETonemap tonemap,
    EPostProcessing postProcessing,
    bool isCropped,
    const Vector2f& cropMin,
    const Vector2f& cropMax
) {
    mShader.bind();
    bindCheckerboardData(pixelSize, checkerSize);
    bindImageData(textureImage, transformImage, exposure, offset, gamma, tonemap);
    mShader.setUniform("hasImage", true);
    mShader.setUniform("postProcessing", static_cast<int>(postProcessing));
    mShader.setUniform("hasReference", false);
    mShader.setUniform("isCropped", isCropped);
    mShader.setUniform("cropMin", cropMin);
    mShader.setUniform("cropMax", cropMax);
    mShader.drawIndexed(GL_TRIANGLES, 0, 2);
}

void UberShader::draw(
    const Vector2f& pixelSize,
    const Vector2f& checkerSize,
    GlTexture* textureImage,
    const Matrix3f& transformImage,
    GlTexture* textureReference,
    const Matrix3f& transformReference,
    float exposure,
    float offset,
    float gamma,
    ETonemap tonemap,
    EMetric metric,
    EPostProcessing postProcessing,
    bool isCropped,
    const Vector2f& cropMin,
    const Vector2f& cropMax
) {
    mShader.bind();
    bindCheckerboardData(pixelSize, checkerSize);
    bindImageData(textureImage, transformImage, exposure, offset, gamma, tonemap);
    bindReferenceData(textureReference, transformReference, metric);
    mShader.setUniform("hasImage", true);
    mShader.setUniform("postProcessing", static_cast<int>(postProcessing));
    mShader.setUniform("hasReference", true);
    mShader.setUniform("isCropped", isCropped);
    mShader.setUniform("cropMin", cropMin);
    mShader.setUniform("cropMax", cropMax);
    mShader.drawIndexed(GL_TRIANGLES, 0, 2);
}

void UberShader::bindCheckerboardData(const Vector2f& pixelSize, const Vector2f& checkerSize) {
    mShader.setUniform("pixelSize", pixelSize);
    mShader.setUniform("checkerSize", checkerSize);
    mShader.setUniform("bgColor", mBackgroundColor);
}

void UberShader::bindImageData(
    GlTexture* textureImage,
    const Matrix3f& transformImage,
    float exposure,
    float offset,
    float gamma,
    ETonemap tonemap
) {
    glActiveTexture(GL_TEXTURE0);
    textureImage->bind();

    mShader.bind();
    mShader.setUniform("image", 0);
    mShader.setUniform("imageTransform", transformImage);

    mShader.setUniform("exposure", exposure);
    mShader.setUniform("offset", offset);
    mShader.setUniform("gamma", gamma);
    mShader.setUniform("tonemap", static_cast<int>(tonemap));

    glActiveTexture(GL_TEXTURE2);
    mColorMap.bind();
    mShader.setUniform("colormap", 2);
}

void UberShader::bindReferenceData(
    GlTexture* textureReference,
    const Matrix3f& transformReference,
    EMetric metric
) {
    glActiveTexture(GL_TEXTURE1);
    textureReference->bind();

    mShader.setUniform("reference", 1);
    mShader.setUniform("referenceTransform", transformReference);

    mShader.setUniform("metric", static_cast<int>(metric));
}

TEV_NAMESPACE_END
