precision mediump float;
uniform sampler2D tex;
uniform float offset;
uniform vec2 halfpixel;

uniform vec2 refractionRectSize;
uniform float refractionEdgeSizePixels;
uniform float refractionCornerRadiusPixels;
uniform float refractionStrength;
uniform float refractionNormalPow;
uniform float refractionRGBFringing;
uniform int refractionTextureRepeatMode;
uniform int refractionMode; // 0: Basic, 1: Concave

varying vec2 v_uv;

vec2 applyTextureRepeatMode(vec2 coord) {
  if (refractionTextureRepeatMode == 0) {
    return clamp(coord, 0.0, 1.0);
  } else if (refractionTextureRepeatMode == 1) {
    // mirror repeat
    vec2 flip = mod(coord, 2.0);
    vec2 result;

    if (flip.x > 1.0) {
      result.x = 1.0 - mod(coord.x, 1.0);
    } else {
      result.x = mod(coord.x, 1.0);
    }

    if (flip.y > 1.0) {
      result.y = 1.0 - mod(coord.y, 1.0);
    } else {
      result.y = mod(coord.y, 1.0);
    }

    return result;
  }
  return coord;
}

// source: https://iquilezles.org/articles/distfunctions2d/
float roundedRectangleDist(vec2 p, vec2 b, float r) {
  vec2 q = abs(p) - b + r;
  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main(void) {
  vec2 halfRefractionRectSize = 0.5 * refractionRectSize;
  vec2 position = v_uv * refractionRectSize - halfRefractionRectSize.xy;
  float cornerR = min(refractionCornerRadiusPixels, min(halfRefractionRectSize.x, halfRefractionRectSize.y));

  vec4 sum = vec4(0.0);
  vec2 coordR, coordG, coordB;

  if (refractionMode == 1) {
      // Concave: lens-like radial mapping with RGB fringing
      float distConcave = roundedRectangleDist(position, halfRefractionRectSize, cornerR);
      float fringing = refractionRGBFringing * 0.3;
      float baseStrength = 0.2 * refractionStrength;

      // Edge proximity shaping
      float edgeProximity = clamp(1.0 + distConcave / refractionEdgeSizePixels, 0.0, 1.0);
      float shaped = sin(pow(edgeProximity, refractionNormalPow) * 1.57079632679);

      vec2 fromCenter = v_uv - vec2(0.5);
      float scaleR = 1.0 - shaped * baseStrength * (1.0 + fringing);
      float scaleG = 1.0 - shaped * baseStrength;
      float scaleB = 1.0 - shaped * baseStrength * (1.0 - fringing);

      coordR = applyTextureRepeatMode(vec2(0.5) + fromCenter * scaleR);
      coordG = applyTextureRepeatMode(vec2(0.5) + fromCenter * scaleG);
      coordB = applyTextureRepeatMode(vec2(0.5) + fromCenter * scaleB);
  } else {
      // Basic: convex/bulge-like along inward normal from the rounded-rect edge
      float distBulge = roundedRectangleDist(position, halfRefractionRectSize, refractionEdgeSizePixels);
      float concaveFactor = pow(clamp(1.0 + distBulge / refractionEdgeSizePixels, 0.0, 1.0), refractionNormalPow);

      // Initial 2D normal
      const float h = 1.0;
      vec2 gradient = vec2(
          roundedRectangleDist(position + vec2(h, 0.0), halfRefractionRectSize, refractionEdgeSizePixels) - roundedRectangleDist(position - vec2(h, 0.0), halfRefractionRectSize, refractionEdgeSizePixels),
          roundedRectangleDist(position + vec2(0.0, h), halfRefractionRectSize, refractionEdgeSizePixels) - roundedRectangleDist(position - vec2(0.0, h), halfRefractionRectSize, refractionEdgeSizePixels)
      );

      vec2 normal = length(gradient) > 1e-6 ? -normalize(gradient) : vec2(0.0, 1.0);

      float finalStrength = 0.2 * concaveFactor * refractionStrength;

      // Different refraction offsets for each color channel
      float fringingFactor = refractionRGBFringing * 0.3;
      vec2 refractOffsetR = normal.xy * (finalStrength * (1.0 + fringingFactor));
      vec2 refractOffsetG = normal.xy * finalStrength;
      vec2 refractOffsetB = normal.xy * (finalStrength * (1.0 - fringingFactor));

      coordR = applyTextureRepeatMode(v_uv - refractOffsetR);
      coordG = applyTextureRepeatMode(v_uv - refractOffsetG);
      coordB = applyTextureRepeatMode(v_uv - refractOffsetB);
  }

  // Manual sampling loop (8 samples, total weight 12.0)
  vec2 off;

  // (-2, 0), weight 1
  off = vec2(-halfpixel.x * 2.0, 0.0) * offset;
  sum.r += texture2D(tex, coordR + off).r;
  sum.g += texture2D(tex, coordG + off).g;
  sum.b += texture2D(tex, coordB + off).b;
  sum.a += texture2D(tex, coordG + off).a;

  // (-1, 1), weight 2
  off = vec2(-halfpixel.x, halfpixel.y) * offset;
  sum.r += texture2D(tex, coordR + off).r * 2.0;
  sum.g += texture2D(tex, coordG + off).g * 2.0;
  sum.b += texture2D(tex, coordB + off).b * 2.0;
  sum.a += texture2D(tex, coordG + off).a * 2.0;

  // (0, 2), weight 1
  off = vec2(0.0, halfpixel.y * 2.0) * offset;
  sum.r += texture2D(tex, coordR + off).r;
  sum.g += texture2D(tex, coordG + off).g;
  sum.b += texture2D(tex, coordB + off).b;
  sum.a += texture2D(tex, coordG + off).a;

  // (1, 1), weight 2
  off = vec2(halfpixel.x, halfpixel.y) * offset;
  sum.r += texture2D(tex, coordR + off).r * 2.0;
  sum.g += texture2D(tex, coordG + off).g * 2.0;
  sum.b += texture2D(tex, coordB + off).b * 2.0;
  sum.a += texture2D(tex, coordG + off).a * 2.0;

  // (2, 0), weight 1
  off = vec2(halfpixel.x * 2.0, 0.0) * offset;
  sum.r += texture2D(tex, coordR + off).r;
  sum.g += texture2D(tex, coordG + off).g;
  sum.b += texture2D(tex, coordB + off).b;
  sum.a += texture2D(tex, coordG + off).a;

  // (1, -1), weight 2
  off = vec2(halfpixel.x, -halfpixel.y) * offset;
  sum.r += texture2D(tex, coordR + off).r * 2.0;
  sum.g += texture2D(tex, coordG + off).g * 2.0;
  sum.b += texture2D(tex, coordB + off).b * 2.0;
  sum.a += texture2D(tex, coordG + off).a * 2.0;

  // (0, -2), weight 1
  off = vec2(0.0, -halfpixel.y * 2.0) * offset;
  sum.r += texture2D(tex, coordR + off).r;
  sum.g += texture2D(tex, coordG + off).g;
  sum.b += texture2D(tex, coordB + off).b;
  sum.a += texture2D(tex, coordG + off).a;

  // (-1, -1), weight 2
  off = vec2(-halfpixel.x, -halfpixel.y) * offset;
  sum.r += texture2D(tex, coordR + off).r * 2.0;
  sum.g += texture2D(tex, coordG + off).g * 2.0;
  sum.b += texture2D(tex, coordB + off).b * 2.0;
  sum.a += texture2D(tex, coordG + off).a * 2.0;

  gl_FragColor = sum / 12.0;
}
