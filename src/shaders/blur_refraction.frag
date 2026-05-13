precision mediump float;
uniform sampler2D tex;
uniform float offset;
uniform vec2 halfpixel;

uniform vec2 refraction_rect_size;
uniform float refraction_edge_size_pixels;
uniform float refraction_corner_radius_pixels;
uniform float refraction_strength;
uniform float refraction_normal_pow;
uniform float refraction_RGB_fringing;
uniform int refraction_texture_repeat_mode;
uniform int refraction_mode; // 0: Basic, 1: Concave

varying vec2 v_uv;

vec2 applyTextureRepeatMode(vec2 coord) {
  if (refraction_texture_repeat_mode == 0) {
    return clamp(coord, 0.0, 1.0);
  } else if (refraction_texture_repeat_mode == 1) {
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
  vec2 halfRefractionRectSize = 0.5 * refraction_rect_size;
  vec2 position = v_uv * refraction_rect_size - halfRefractionRectSize.xy;
  float cornerR = min(refraction_corner_radius_pixels, min(halfRefractionRectSize.x, halfRefractionRectSize.y));

  vec4 sum = vec4(0.0);
  vec2 coordR, coordG, coordB;

  if (refraction_mode == 1) {
    float distConcave = roundedRectangleDist(position, halfRefractionRectSize, cornerR);
    float fringing = refraction_RGB_fringing * 0.3;
    float baseStrength = 0.2 * refraction_strength;

    // Edge proximity shaping
    float edgeProximity = clamp(1.0 + distConcave / refraction_edge_size_pixels, 0.0, 1.0);
    float shaped = sin(pow(edgeProximity, refraction_normal_pow) * 1.57079632679);

    vec2 fromCenter = v_uv - vec2(0.5);
    float scaleR = 1.0 - shaped * baseStrength * (1.0 + fringing);
    float scaleG = 1.0 - shaped * baseStrength;
    float scaleB = 1.0 - shaped * baseStrength * (1.0 - fringing);

    coordR = applyTextureRepeatMode(vec2(0.5) + fromCenter * scaleR);
    coordG = applyTextureRepeatMode(vec2(0.5) + fromCenter * scaleG);
    coordB = applyTextureRepeatMode(vec2(0.5) + fromCenter * scaleB);
  } else {
    float distBulge = roundedRectangleDist(position, halfRefractionRectSize, refraction_edge_size_pixels);
    float concaveFactor = pow(clamp(1.0 + distBulge / refraction_edge_size_pixels, 0.0, 1.0), refraction_normal_pow);

    // Numerical gradient of the SDF gives the surface normal
    const float h = 1.0;
    vec2 gradient = vec2(
        roundedRectangleDist(position + vec2(h, 0.0), halfRefractionRectSize, refraction_edge_size_pixels) - roundedRectangleDist(position - vec2(h, 0.0), halfRefractionRectSize, refraction_edge_size_pixels),
        roundedRectangleDist(position + vec2(0.0, h), halfRefractionRectSize, refraction_edge_size_pixels) - roundedRectangleDist(position - vec2(0.0, h), halfRefractionRectSize, refraction_edge_size_pixels)
      );

    vec2 normal = length(gradient) > 1e-6 ? -normalize(gradient) : vec2(0.0, 1.0);

    float finalStrength = 0.2 * concaveFactor * refraction_strength;

    float fringingFactor = refraction_RGB_fringing * 0.3;
    vec2 refractOffsetR = normal.xy * (finalStrength * (1.0 + fringingFactor));
    vec2 refractOffsetG = normal.xy * finalStrength;
    vec2 refractOffsetB = normal.xy * (finalStrength * (1.0 - fringingFactor));

    coordR = applyTextureRepeatMode(v_uv - refractOffsetR);
    coordG = applyTextureRepeatMode(v_uv - refractOffsetG);
    coordB = applyTextureRepeatMode(v_uv - refractOffsetB);
  }

  // Manual sampling loop (8 samples, total weight 12.0)
  vec2 off;

  off = vec2(-halfpixel.x * 2.0, 0.0) * offset;
  sum.r += texture2D(tex, coordR + off).r;
  sum.g += texture2D(tex, coordG + off).g;
  sum.b += texture2D(tex, coordB + off).b;
  sum.a += texture2D(tex, coordG + off).a;

  off = vec2(-halfpixel.x, halfpixel.y) * offset;
  sum.r += texture2D(tex, coordR + off).r * 2.0;
  sum.g += texture2D(tex, coordG + off).g * 2.0;
  sum.b += texture2D(tex, coordB + off).b * 2.0;
  sum.a += texture2D(tex, coordG + off).a * 2.0;

  off = vec2(0.0, halfpixel.y * 2.0) * offset;
  sum.r += texture2D(tex, coordR + off).r;
  sum.g += texture2D(tex, coordG + off).g;
  sum.b += texture2D(tex, coordB + off).b;
  sum.a += texture2D(tex, coordG + off).a;

  off = vec2(halfpixel.x, halfpixel.y) * offset;
  sum.r += texture2D(tex, coordR + off).r * 2.0;
  sum.g += texture2D(tex, coordG + off).g * 2.0;
  sum.b += texture2D(tex, coordB + off).b * 2.0;
  sum.a += texture2D(tex, coordG + off).a * 2.0;

  off = vec2(halfpixel.x * 2.0, 0.0) * offset;
  sum.r += texture2D(tex, coordR + off).r;
  sum.g += texture2D(tex, coordG + off).g;
  sum.b += texture2D(tex, coordB + off).b;
  sum.a += texture2D(tex, coordG + off).a;

  off = vec2(halfpixel.x, -halfpixel.y) * offset;
  sum.r += texture2D(tex, coordR + off).r * 2.0;
  sum.g += texture2D(tex, coordG + off).g * 2.0;
  sum.b += texture2D(tex, coordB + off).b * 2.0;
  sum.a += texture2D(tex, coordG + off).a * 2.0;

  off = vec2(0.0, -halfpixel.y * 2.0) * offset;
  sum.r += texture2D(tex, coordR + off).r;
  sum.g += texture2D(tex, coordG + off).g;
  sum.b += texture2D(tex, coordB + off).b;
  sum.a += texture2D(tex, coordG + off).a;

  off = vec2(-halfpixel.x, -halfpixel.y) * offset;
  sum.r += texture2D(tex, coordR + off).r * 2.0;
  sum.g += texture2D(tex, coordG + off).g * 2.0;
  sum.b += texture2D(tex, coordB + off).b * 2.0;
  sum.a += texture2D(tex, coordG + off).a * 2.0;

  vec4 blurred = sum / 12.0;
  gl_FragColor = blurred;
}
