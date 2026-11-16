#version 450
// Fire Emblem Engage, fragment shader d31ff45e8a33bdf1: the battle scenes' grass blades lit by point lights (the variant the game draws for about a second after a hit,
// while a hit's light is in its light table; its vertex shader a9efe56cd3ea6a7c). Translated, it drew the blades darker than the unlit variant 1912f94e9c44fab3 does
// (the knight's axe hit in chapter 2: the whole lawn went dark and came back at once; the Switch shows no change), though their code is the same but for the
// point-light loop, and the loop adds nothing there (a debug build showed every blade with lights counted and none added, no NaN). Not found why.
// This is 1912f94e9c44fab3 (spirv-cross of the translation) moved onto this shader's bindings: registers 1 and up one higher (register 1 is the light table here),
// register 0 indices 3 and up one higher (the light count bound at c3[3].x), the texture and buffer binding numbers this shader's. The blades lose the hit's light
// (the ground under them keeps it). Compiled with glslc -fshader-stage=frag --target-env=vulkan1.1 into android_spv.d31ff45e8a33bdf1.spv (shader_vk.py takes it)

layout(set = 0, binding = 0, std140) uniform support_buffer
{
    uint _m0;
    uint _m1[8];
    vec4 _m2;
    vec4 _m3;
    int _m4;
    float _m5[73];
    ivec4 _m6;
    int _m7;
} support_buffer_1;

layout(set = 0, binding = 1, std140) uniform fp_c7
{
    vec4 _m0[4096];
} fp_c7_1;

layout(set = 0, binding = 10, std140) uniform fp_c6
{
    vec4 _m0[4096];
} fp_c6_1;

layout(set = 0, binding = 11, std140) uniform fp_c3
{
    vec4 _m0[4096];
} fp_c3_1;

layout(set = 0, binding = 3, std140) uniform fp_c5
{
    vec4 _m0[4096];
} fp_c5_1;

layout(set = 0, binding = 5) uniform sampler2D fp_t_cb196610_1057805E;
layout(set = 0, binding = 6) uniform sampler2DShadow fp_t_cb196610_10580060;
layout(set = 0, binding = 7) uniform sampler2D fp_t_cb196610_1057005C;
layout(set = 0, binding = 8) uniform sampler2D fp_t_cb196610_1056805A;
layout(set = 0, binding = 9) uniform sampler2D fp_t_cb196610_10588062;

layout(location = 0) in vec4 _44;
layout(location = 2) in vec4 _46;
layout(location = 1) in vec4 _48;
layout(location = 7) in vec4 _50;
layout(location = 6) in vec4 _52;
layout(location = 8) in vec4 _54;
layout(location = 4) in vec4 _56;
layout(location = 0) out vec4 _59;

void main()
{
    vec2 _227 = vec2(_44.x, _44.y);
    vec4 _66 = texture(fp_t_cb196610_1057805E, mix(_227, vec2(0.0), isnan(_227))).xyzw;
    float _69 = _66.w;
    float _71 = _46.x;
    float _73 = _46.y;
    float _75 = _46.z;
    if (_69 < fp_c6_1._m0[6].z)
    {
        discard;
    }
    float _77 = _75 + (-fp_c7_1._m0[21].z);
    float _79 = fma(_75 + (-fp_c7_1._m0[20].z), _75 + (-fp_c7_1._m0[20].z), fma(_73 + (-fp_c7_1._m0[20].y), _73 + (-fp_c7_1._m0[20].y), (_71 + (-fp_c7_1._m0[20].x)) * (_71 + (-fp_c7_1._m0[20].x))));
    float _81 = fma(_77, _77, fma(_73 + (-fp_c7_1._m0[21].y), _73 + (-fp_c7_1._m0[21].y), (_71 + (-fp_c7_1._m0[21].x)) * (_71 + (-fp_c7_1._m0[21].x))));
    bool _85 = _79 < fp_c7_1._m0[24].x;
    bool _87 = _81 < fp_c7_1._m0[24].y;
    float _89 = _77;
    if (_85)
    {
        _89 = -1.0;
    }
    float _91 = _89;
    if (!_85)
    {
        _91 = -0.0;
    }
    float _93 = _75 + (-fp_c7_1._m0[22].z);
    float _95 = fma(_93, _93, fma(_73 + (-fp_c7_1._m0[22].y), _73 + (-fp_c7_1._m0[22].y), (_71 + (-fp_c7_1._m0[22].x)) * (_71 + (-fp_c7_1._m0[22].x))));
    float _97 = _93;
    if (_87)
    {
        _97 = -1.0;
    }
    float _99 = _97;
    if (!_87)
    {
        _99 = -0.0;
    }
    bool _101 = _95 < fp_c7_1._m0[24].z;
    float _103 = _75 + (-fp_c7_1._m0[23].z);
    float _105 = _103;
    if (_101)
    {
        _105 = -1.0;
    }
    float _107 = _105;
    if (!_101)
    {
        _107 = -0.0;
    }
    float _109 = (-(max(0.0, _107 + float(fma(_103, _103, fma(_73 + (-fp_c7_1._m0[23].y), _73 + (-fp_c7_1._m0[23].y), (_71 + (-fp_c7_1._m0[23].x)) * (_71 + (-fp_c7_1._m0[23].x)))) < fp_c7_1._m0[24].w)) + fma(max(0.0, _99 + float(_95 < fp_c7_1._m0[24].z)), 2.0, fma(max(0.0, _91 + float(_81 < fp_c7_1._m0[24].y)), 3.0, float(_79 < fp_c7_1._m0[24].x) * 4.0)))) + 4.0;
    int _112 = int(uint(max(trunc(_109), 0.0))) << 6;
    uint _115 = uint(int(uint(_112 + 16) >> uint(2)));
    int _117 = int(_115) + 1;
    uint _119 = uint(int(uint(_112) >> uint(2)));
    int _121 = int(_119) + 1;
    uint _123 = uint(int(uint(_112 + 32) >> uint(2)));
    int _125 = int(_123) + 1;
    uint _127 = uint(int(uint(_112 + 48) >> uint(2)));
    int _129 = int(_127) + 1;
    uint _131 = uint(int(uint(_112 + 40) >> uint(2)));
    uint _133 = uint(int(uint(_112 + 56) >> uint(2)));
    float _135 = fp_c7_1._m0[int(uint(int(_133)) >> uint(2))][int(_133) & 3];
    float _137 = fma(_75, fp_c7_1._m0[int(uint(int(_123)) >> uint(2))][int(_123) & 3], fma(_71, fp_c7_1._m0[int(uint(int(_119)) >> uint(2))][int(_119) & 3], _73 * fp_c7_1._m0[int(uint(int(_115)) >> uint(2))][int(_115) & 3])) + fp_c7_1._m0[int(uint(int(_127)) >> uint(2))][int(_127) & 3];
    uint _139 = uint(int(uint(_112 + 24) >> uint(2)));
    float _141 = fma(_75, fp_c7_1._m0[int(uint(_125) >> uint(2))][_125 & 3], fma(_71, fp_c7_1._m0[int(uint(_121) >> uint(2))][_121 & 3], _73 * fp_c7_1._m0[int(uint(_117) >> uint(2))][_117 & 3])) + fp_c7_1._m0[int(uint(_129) >> uint(2))][_129 & 3];
    uint _143 = uint(int(uint(_112 + 8) >> uint(2)));
    float _145 = fma(_75, fp_c7_1._m0[int(uint(int(_131)) >> uint(2))][int(_131) & 3], fma(_71, fp_c7_1._m0[int(uint(int(_143)) >> uint(2))][int(_143) & 3], _73 * fp_c7_1._m0[int(uint(int(_139)) >> uint(2))][int(_139) & 3]));
    float _147 = _145 + _135;
    float _149 = _48.x;
    float _151 = _48.y;
    float _153 = fp_c3_1._m0[4].x * fp_c3_1._m0[7].z;
    float _155 = fp_c3_1._m0[4].x * fp_c3_1._m0[7].w;
    float _157 = 1.0 / fp_c3_1._m0[8].x;
    vec2 _648 = vec2(_149, _151);
    vec4 _159 = texture(fp_t_cb196610_1057005C, mix(_648, vec2(0.0), isnan(_648))).xyzw;
    vec2 _656 = vec2(_149, _151);
    vec4 _161 = texture(fp_t_cb196610_1056805A, mix(_656, vec2(0.0), isnan(_656))).xyzw;
    vec2 _686 = vec2(fma(_71 + (-fp_c3_1._m0[7].x), _157, _153 + (-floor(_153))), fma(_75 + (-fp_c3_1._m0[7].y), _157, _155 + (-floor(_155))));
    vec4 _163 = texture(fp_t_cb196610_10588062, mix(_686, vec2(0.0), isnan(_686))).xyzw;
    float _165 = _50.x;
    bool _167 = (_147 >= 1.0) || (_145 <= (-_135));
    float _169 = _50.z;
    bool _171 = _109 == 4.0;
    float _173 = (-fp_c7_1._m0[29].x) + 1.0;
    float _175 = clamp(_44.z, 0.0, 1.0) * clamp(_44.z, 0.0, 1.0);
    float _177 = _50.y;
    float _179 = _54.y;
    float _181 = _54.x;
    float _183 = _54.z;
    float _185 = fp_c7_1._m0[29].z;
    float _187 = _173;
    if (_171)
    {
        _185 = 1.0;
    }
    float _189 = _185;
    if (!_171)
    {
        _189 = clamp(fma(fma(_75 + (-fp_c3_1._m0[5].z), _75 + (-fp_c3_1._m0[5].z), fma(_46.y + (-fp_c3_1._m0[5].y), _46.y + (-fp_c3_1._m0[5].y), (_71 + (-fp_c3_1._m0[5].x)) * (_71 + (-fp_c3_1._m0[5].x)))), fp_c7_1._m0[29].z, fp_c7_1._m0[29].w), 0.0, 1.0) * clamp(fma(fma(_75 + (-fp_c3_1._m0[5].z), _75 + (-fp_c3_1._m0[5].z), fma(_46.y + (-fp_c3_1._m0[5].y), _46.y + (-fp_c3_1._m0[5].y), (_71 + (-fp_c3_1._m0[5].x)) * (_71 + (-fp_c3_1._m0[5].x)))), fp_c7_1._m0[29].z, fp_c7_1._m0[29].w), 0.0, 1.0);
    }
    float _191 = clamp(_54.w + (-0.0), 0.0, 1.0);
    float _193 = fma(_191, fma(_66.x, fma(_175, _52.x + (-_165), _165), -_181), _181);
    float _195 = fma(_191, fma(_66.y, fma(_175, _52.y + (-_177), _177), -_179), _179);
    float _197 = fma(_191, fma(_66.z, fma(_175, _52.z + (-_169), _169), -_183), _183);
    if (_167)
    {
        _187 = 1.0;
    }
    float _199 = 1.0 / max(_161.w, 0.01000213623046875);
    float _201 = _187;
    if (!_167)
    {
        vec2 _893 = vec2(_137 + fp_c7_1._m0[28].x, _141 + fp_c7_1._m0[28].y);
        vec2 _912 = vec2(_137 + fp_c7_1._m0[27].x, _141 + fp_c7_1._m0[27].y);
        vec2 _930 = vec2(_137 + fp_c7_1._m0[26].x, _141 + fp_c7_1._m0[26].y);
        vec2 _948 = vec2(_137 + fp_c7_1._m0[25].x, _141 + fp_c7_1._m0[25].y);
        _201 = fma(fma(textureLod(fp_t_cb196610_10580060, vec3(mix(_893, vec2(0.0), isnan(_893)), _147 + fp_c7_1._m0[28].z), 0.0), 0.25, fma(textureLod(fp_t_cb196610_10580060, vec3(mix(_912, vec2(0.0), isnan(_912)), _147 + fp_c7_1._m0[27].z), 0.0), 0.25, fma(textureLod(fp_t_cb196610_10580060, vec3(mix(_930, vec2(0.0), isnan(_930)), _147 + fp_c7_1._m0[26].z), 0.0), 0.25, textureLod(fp_t_cb196610_10580060, vec3(mix(_948, vec2(0.0), isnan(_948)), _147 + fp_c7_1._m0[25].z), 0.0) * 0.25))), fp_c7_1._m0[29].x, _173);
    }
    float _203 = _201;
    float _205 = clamp((-0.0) + fp_c3_1._m0[0].y, 0.0, 1.0);
    float _207 = min(fma((-_203) + 1.0, _189, _203), fma(_159.w + (-1.0), fp_c3_1._m0[2].w, fma(_159.z + (-1.0), fp_c3_1._m0[2].z, fma(_159.y + (-1.0), fp_c3_1._m0[2].y, (_159.x + (-1.0)) * fp_c3_1._m0[2].x))) + 1.0) * fp_c5_1._m0[10].z;
    float _209 = _56.x;
    float _211 = max(fma(_207 * fp_c3_1._m0[1].x, _205, _161.x * _199), fp_c6_1._m0[6].w);
    float _213 = max(fma(_207 * fp_c3_1._m0[1].y, _205, _161.y * _199), fp_c6_1._m0[6].w);
    float _215 = max(fma(_207 * fp_c3_1._m0[1].z, _205, _161.z * _199), fp_c6_1._m0[6].w);
    float _217 = _163.w * fp_c3_1._m0[8].y;
    _59.x = fma(fma(fma(_193, -_211, _163.x), _217, _193 * _211) + (-fp_c3_1._m0[6].x), _209, fp_c3_1._m0[6].x);
    _59.y = fma(fma(fma(_195, -_213, _163.y), _217, _195 * _213) + (-fp_c3_1._m0[6].y), _209, fp_c3_1._m0[6].y);
    _59.z = fma(fma(fma(_197, -_215, _163.z), _217, _197 * _215) + (-fp_c3_1._m0[6].z), _209, fp_c3_1._m0[6].z);
    _59.w = _69;
}

