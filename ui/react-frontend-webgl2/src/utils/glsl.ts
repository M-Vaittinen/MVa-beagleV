export const VERT = /*glsl*/ `#version 300 es
uniform int  u_iOffset;
uniform float u_xMin;
uniform float u_xMax;
uniform float u_yMin;
uniform float u_yMax;
in float a_y;
void main() {
  float x  = float(gl_VertexID - u_iOffset);
  float nx = (x   - u_xMin) / (u_xMax - u_xMin) * 2.0 - 1.0;
  float ny = (a_y - u_yMin) / (u_yMax - u_yMin) * 2.0 - 1.0;
  gl_Position = vec4(nx, ny, 0.0, 1.0);
}`;

export const FRAG = /*glsl*/ `#version 300 es
precision mediump float;
uniform vec3 u_color;
out vec4 outColor;
void main() { outColor = vec4(u_color, 1.0); }`;

export function compileShader(
  gl: WebGL2RenderingContext,
  type: GLenum,
  src: string,
): WebGLShader {
  const s = gl.createShader(type);
  if (!s) throw new Error("Failed to create shader");
  gl.shaderSource(s, src);
  gl.compileShader(s);
  if (!gl.getShaderParameter(s, gl.COMPILE_STATUS))
    throw new Error(gl.getShaderInfoLog(s) ?? "Shader compilation failed");
  return s;
}
