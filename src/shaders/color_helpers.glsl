// Color space and brightness helper functions for shaders (based on shaders from Hyprland)

precision mediump float;

// Perceived brightness weights (standard luminance coefficients)
// Source: http://alienryderflex.com/hsp.html
const mediump float LUM_R = 0.299;
const mediump float LUM_G = 0.587;
const mediump float LUM_B = 0.114;

const mediump float VIBRANCY_A = 0.93;
const mediump float VIBRANCY_B = 0.11;
const mediump float VIBRANCY_C = 0.66;

// Simple hash function for noise generation
// From: https://www.shadertoy.com/view/4djSRW
mediump float hash(mediump vec2 p) {
  mediump vec3 p3 = fract(vec3(p.xyx) * 1689.1984);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}

// Double circle sigmoid curve function
mediump float doubleCircleSigmoid(mediump float x, mediump float a) {
  a = clamp(a, 0.0, 1.0);

  mediump float y = 0.0;
  if (x <= a) {
    y = a - sqrt(max(a * a - x * x, 0.0));
  } else {
    y = a + sqrt(max(pow(1.0 - a, 2.0) - pow(x - 1.0, 2.0), 0.0));
  }
  return y;
}

// Helper for HSL to RGB conversion (must come before hsl2rgb which uses it)
mediump float hue2rgb(mediump float p, mediump float q, mediump float t) {
  if (t < 0.0) t += 1.0;
  if (t > 1.0) t -= 1.0;
  if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
  if (t < 1.0 / 2.0) return q;
  if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
  return p;
}

// Convert RGB to HSL (Hue, Saturation, Lightness)
mediump vec3 rgb2hsl(mediump vec3 col) {
  mediump float maxc = max(col.r, max(col.g, col.b));
  mediump float minc = min(col.r, min(col.g, col.b));
  mediump float delta = maxc - minc;

  mediump float lum = (minc + maxc) * 0.5;
  mediump float sat = 0.0;
  mediump float hue = 0.0;

  if (lum > 0.0 && lum < 1.0) {
    mediump float mul = (lum < 0.5) ? lum : (1.0 - lum);
    sat = delta / (mul * 2.0);
  }

  if (delta > 0.0) {
    if (maxc == col.r) {
      hue = mod((col.g - col.b) / delta, 6.0) / 6.0;
    } else if (maxc == col.g) {
      hue = ((col.b - col.r) / delta + 2.0) / 6.0;
    } else {
      hue = ((col.r - col.g) / delta + 4.0) / 6.0;
    }
  }

  return vec3(hue, sat, lum);
}

// Convert HSL back to RGB
mediump vec3 hsl2rgb(mediump vec3 col) {
  mediump float hue = col.x;
  mediump float sat = col.y;
  mediump float lum = col.z;

  mediump vec3 rgb;

  if (sat == 0.0) {
    rgb = vec3(lum);
  } else {
    mediump float q = (lum < 0.5) ? (lum * (1.0 + sat)) : (lum + sat - lum * sat);
    mediump float p = 2.0 * lum - q;

    rgb.r = hue2rgb(p, q, hue + 1.0 / 3.0);
    rgb.g = hue2rgb(p, q, hue);
    rgb.b = hue2rgb(p, q, hue - 1.0 / 3.0);
  }

  return rgb;
}

// Calculate perceived brightness of RGB color
mediump float getPerceivedBrightness(mediump vec3 rgb) {
  return sqrt(rgb.r * rgb.r * LUM_R + rgb.g * rgb.g * LUM_G + rgb.b * rgb.b * LUM_B);
}

// Add noise to reduce banding artifacts
mediump vec3 addNoise(mediump vec3 color, mediump vec2 uv, mediump float strength) {
  mediump float noiseVal = hash(uv) - 0.5;
  return color + (noiseVal * strength);
}

mediump vec3 applyVibrancy(mediump vec3 rgb, mediump float vibrancy, mediump float vibrancy_darkness, mediump float passes) {
  if (vibrancy <= 0.0) {
    return rgb;
  }

  mediump float vibrancy_darkness1 = 1.0 - vibrancy_darkness;
  mediump vec3 hsl = rgb2hsl(rgb);

  mediump float perceivedBrightness = doubleCircleSigmoid(
      sqrt(rgb.r * rgb.r * LUM_R + rgb.g * rgb.g * LUM_G + rgb.b * rgb.b * LUM_B),
      0.8 * vibrancy_darkness1
    );

  mediump float b1 = VIBRANCY_B * vibrancy_darkness1;
  mediump float boostBase = hsl[1] > 0.0 ?
    smoothstep(b1 - VIBRANCY_C * 0.5, b1 + VIBRANCY_C * 0.5,
      1.0 - (pow(1.0 - hsl[1] * cos(VIBRANCY_A), 2.0) +
          pow(1.0 - perceivedBrightness * sin(VIBRANCY_A), 2.0))) : 0.0;

  mediump float saturation = clamp(hsl[1] + (boostBase * vibrancy) / passes, 0.0, 1.0);
  return hsl2rgb(vec3(hsl[0], saturation, hsl[2]));
}

mediump vec3 applyContrast(mediump vec3 rgb, mediump float contrast) {
  if (contrast == 1.0) {
    return rgb;
  }

  return (rgb - 0.5) * contrast + 0.5;
}

mediump vec3 applyBrightness(mediump vec3 rgb, mediump float brightness) {
  if (brightness == 1.0) {
    return rgb;
  }

  return rgb * brightness;
}

mediump vec3 applyBrightnessContrast(mediump vec3 rgb, mediump float brightness, mediump float contrast) {
  rgb = applyBrightness(rgb, brightness);
  rgb = applyContrast(rgb, contrast);
  return rgb;
}
