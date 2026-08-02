const float LUM_R = 0.299;
const float LUM_G = 0.587;
const float LUM_B = 0.114;

const float VIBRANCY_A = 0.93;
const float VIBRANCY_B = 0.11;
const float VIBRANCY_C = 0.66;

float hash(vec2 p) {
  vec3 p3 = fract(vec3(p.xyx) * 1689.1984);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}

float doubleCircleSigmoid(float x, float a) {
  a = clamp(a, 0.0, 1.0);
  float y = 0.0;
  if (x <= a)
    y = a - sqrt(max(a * a - x * x, 0.0));
  else
    y = a + sqrt(max(pow(1.0 - a, 2.0) - pow(x - 1.0, 2.0), 0.0));

  return y;
}

float hue2rgb(float p, float q, float t) {
  if (t < 0.0) t += 1.0;
  if (t > 1.0) t -= 1.0;
  if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
  if (t < 1.0 / 2.0) return q;
  if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
  return p;
}

vec3 rgb2hsl(vec3 col) {
  float maxc = max(col.r, max(col.g, col.b));
  float minc = min(col.r, min(col.g, col.b));
  float delta = maxc - minc;
  float lum = (minc + maxc) * 0.5;
  float sat = 0.0;
  float hue = 0.0;
  if (lum > 0.0 && lum < 1.0) {
    float mul = (lum < 0.5) ? lum : (1.0 - lum);
    sat = delta / (mul * 2.0);
  }
  if (delta > 0.0) {
    if (maxc == col.r)
      hue = mod((col.g - col.b) / delta, 6.0) / 6.0;
    else if (maxc == col.g)
      hue = ((col.b - col.r) / delta + 2.0) / 6.0;
    else
      hue = ((col.r - col.g) / delta + 4.0) / 6.0;
  }
  return vec3(hue, sat, lum);
}

vec3 hsl2rgb(vec3 col) {
  float hue = col.x;
  float sat = col.y;
  float lum = col.z;
  vec3 rgb;
  if (sat == 0.0) {
    rgb = vec3(lum);
  } else {
    float q = (lum < 0.5) ? (lum * (1.0 + sat)) : (lum + sat - lum * sat);
    float p = 2.0 * lum - q;
    rgb.r = hue2rgb(p, q, hue + 1.0 / 3.0);
    rgb.g = hue2rgb(p, q, hue);
    rgb.b = hue2rgb(p, q, hue - 1.0 / 3.0);
  }
  return rgb;
}

float getPerceivedBrightness(vec3 rgb) {
  return sqrt(rgb.r * rgb.r * LUM_R + rgb.g * rgb.g * LUM_G + rgb.b * rgb.b * LUM_B);
}

vec3 addNoise(vec3 color, vec2 uv, float strength) {
  float noiseVal = hash(uv) - 0.5;
  return color + (noiseVal * strength);
}

vec3 applyVibrancy(vec3 rgb, float vibrancy, float vibrancy_darkness, float passes) {
  if (vibrancy <= 0.0) return rgb;
  float vibrancy_darkness1 = 1.0 - vibrancy_darkness;
  vec3 hsl = rgb2hsl(rgb);
  float perceivedBrightness = doubleCircleSigmoid(
      sqrt(rgb.r * rgb.r * LUM_R + rgb.g * rgb.g * LUM_G + rgb.b * rgb.b * LUM_B),
      0.8 * vibrancy_darkness1);
  float b1 = VIBRANCY_B * vibrancy_darkness1;
  float boostBase = hsl[1] > 0.0 ?
    smoothstep(b1 - VIBRANCY_C * 0.5, b1 + VIBRANCY_C * 0.5,
      1.0 - (pow(1.0 - hsl[1] * cos(VIBRANCY_A), 2.0) +
          pow(1.0 - perceivedBrightness * sin(VIBRANCY_A), 2.0))) : 0.0;
  float saturation = clamp(hsl[1] + (boostBase * vibrancy) / passes, 0.0, 1.0);
  return hsl2rgb(vec3(hsl[0], saturation, hsl[2]));
}

vec3 applyContrast(vec3 rgb, float contrast) {
  if (contrast == 1.0) return rgb;
  return (rgb - 0.5) * contrast + 0.5;
}

vec3 applyBrightness(vec3 rgb, float brightness) {
  if (brightness == 1.0) return rgb;
  return rgb * brightness;
}

vec3 applyBrightnessContrast(vec3 rgb, float brightness, float contrast) {
  rgb = applyBrightness(rgb, brightness);
  rgb = applyContrast(rgb, contrast);
  return rgb;
}

vec3 applySaturation(vec3 rgb, float saturation) {
  if (saturation == 1.0) return rgb;
  float luma = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
  return mix(vec3(luma), rgb, saturation);
}
