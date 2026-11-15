// Dissidia 012: which stage materials reflect (issue #2), by texture-pack id (psp_refl_table.h). A texture not listed here does not
// reflect (grass, dirt, rock, wood, cloth, characters). Picked by eye from every battle stage's textures (texpack_extract.py on
// PACKAGE.BIN; the comment is the stage model and the texture's index among that model's textures). reflectivity.txt in the game
// folder overrides any entry (`<id> <reflectivity> [roughness] [metal] [water]`, reflectivity 0 turns one off); PSPRECOMP_REFL_MASK_DEBUG=1
// tints what reflects. Columns: reflectivity (strength), roughness (0 mirror .. 1 broad sheen: blur + damped Fresnel), metal (tint),
// water (the reflection follows the animated ripples).
#include "psp_refl_table.h"
#include <iterator>

namespace {
const PspReflEntry k_refl[] = {
    // map/org50
    {0x30d3a0d03f83f104ull, 1.00f, 0.10f, false, true},   // 0 water floor (Order's Sanctuary)
    {0x33e024893ff515c1ull, 1.00f, 0.10f, false, true},   // 1 water floor (Order's Sanctuary)
    // map/fiv00
    {0x31c25dab157eea6dull, 0.90f, 0.10f, false, true},   // 8 water
    {0xb046ecd45540d09bull, 0.50f, 0.30f, false, true},   // 19 waterfall
    // map/sev00
    {0x61f75f1c39bb556full, 0.70f, 0.10f, false, true},   // 4 water with sparkles
    {0x9c705770a6832f7aull, 0.60f, 0.10f, false, true},   // 11 dark water
    {0xc3da7e58b998edd4ull, 0.50f, 0.40f, false, false},   // 12 wet rock vein
    // map/eht00
    {0x948a43e5ae278117ull, 0.80f, 0.10f, false, true},   // 14 deep water
    {0x96e66d6573a4910full, 0.70f, 0.10f, false, true},   // 15 swirling water
    {0x22e96a4f4d6c9246ull, 0.40f, 0.45f, false, false},   // 3 teal polished panels
    {0x65a06bc4cb425954ull, 0.40f, 0.45f, false, false},   // 11 teal polished panels
    {0xcd31308daba0a54aull, 0.40f, 0.45f, false, false},   // 19 teal polished panels
    {0xd69893b015a978e4ull, 0.40f, 0.45f, false, false},   // 21 teal polished panels
    {0xde9a4df83bcee74bull, 0.40f, 0.45f, false, false},   // 23 teal polished panels
    {0xe487e3d6aabbb862ull, 0.40f, 0.45f, false, false},   // 26 teal polished panels
    {0x4cd297fed744e959ull, 0.35f, 0.30f, false, false},   // 8 grey marble
    {0xab08d07b8cd1dd2bull, 0.35f, 0.30f, false, false},   // 16 grey marble
    {0x0ce5c861f1199404ull, 0.40f, 0.40f, true, false},   // 1 gold gears
    {0x332a04427d033839ull, 0.40f, 0.40f, true, false},   // 6 gold gears
    {0x4a60a4266ad85fd4ull, 0.40f, 0.40f, true, false},   // 7 gold gears
    {0xe2f58004ca93d280ull, 0.30f, 0.50f, false, false},   // 25 carved trim
    // map/twe00
    {0xc7a43056a3cbf346ull, 0.80f, 0.10f, false, true},   // 38 water caustics
    {0x122b09292525ae66ull, 0.25f, 0.55f, true, false},   // 6 smooth metal plates
    {0x1d6cb9da5a9ce838ull, 0.25f, 0.55f, true, false},   // 8 smooth metal plates
    {0x2e8d129d2b07c4d4ull, 0.25f, 0.55f, true, false},   // 12 smooth metal plates
    {0x8188b4290bf54209ull, 0.25f, 0.55f, true, false},   // 29 smooth metal plates
    {0x974338e5baea6e97ull, 0.25f, 0.55f, true, false},   // 32 smooth metal plates
    {0xb3b0cb4878cd5ed4ull, 0.25f, 0.55f, true, false},   // 34 smooth metal plates
    // map/thr00
    {0x821347708923088eull, 0.80f, 0.10f, false, true},   // 9 deep blue water
    {0x4a8f1d180fe0cd9eull, 0.70f, 0.15f, false, false},   // 5 ice crystal
    {0xc3ee0c217f2639d1ull, 0.70f, 0.10f, false, false},   // 14 crystal
    {0x4b8c96907a534869ull, 0.60f, 0.10f, false, false},   // 6 green crystal
    {0x25333767961fb9baull, 0.60f, 0.20f, false, false},   // 1 glowing tile
    {0x308d2999e5749ce9ull, 0.50f, 0.30f, false, false},   // 2 purple marble
    {0xaadedf821cf9140eull, 0.50f, 0.45f, false, false},   // 11 polished panels
    {0xad1288db0566480cull, 0.50f, 0.45f, false, false},   // 12 polished panels
    {0x1478272058b7beffull, 0.40f, 0.50f, false, false},   // 0 polished stone
    {0x7154b99d598003b0ull, 0.40f, 0.50f, false, false},   // 8 polished stone
    {0xb1e4a71372cf5b7aull, 0.40f, 0.50f, false, false},   // 13 polished stone
    {0xce1fb82352c4e6beull, 0.40f, 0.50f, false, false},   // 15 polished stone
    {0x31bea33f782a8f19ull, 0.30f, 0.50f, false, false},   // 3 stone
    // map/two00
    {0x10340e86ba28f6daull, 0.45f, 0.10f, false, false},   // 2 pink crystal hex tiles
    {0x19de3ebb54089e8bull, 0.45f, 0.10f, false, false},   // 3 pink crystal hex tiles
    {0x1d3fe15e156c1e18ull, 0.45f, 0.10f, false, false},   // 4 pink crystal hex tiles
    {0x20efaf0d8e5161b7ull, 0.45f, 0.10f, false, false},   // 5 pink crystal hex tiles
    {0x2754eba465494ef1ull, 0.45f, 0.10f, false, false},   // 7 pink crystal hex tiles
    {0x2d1bdb2370ee52f1ull, 0.45f, 0.10f, false, false},   // 8 pink crystal hex tiles
    {0x333198f627f84293ull, 0.45f, 0.10f, false, false},   // 9 pink crystal hex tiles
    {0x5079c9965429a449ull, 0.45f, 0.10f, false, false},   // 14 pink crystal hex tiles
    {0x55526d9bc73b5a9eull, 0.45f, 0.10f, false, false},   // 15 pink crystal hex tiles
    {0xce70e69ed88a8f68ull, 0.45f, 0.10f, false, false},   // 23 pink crystal hex tiles
    {0xda66d07b34fe26f1ull, 0.45f, 0.10f, false, false},   // 24 pink crystal hex tiles
    {0xdb1c7c5ea8ae2d2dull, 0.45f, 0.10f, false, false},   // 25 pink crystal hex tiles
    {0x0a987e3f2e882996ull, 0.70f, 0.10f, false, false},   // 1 blue glossy floor
    {0xcc9a2b1899d3850aull, 0.70f, 0.10f, false, false},   // 22 crystal shards
    {0x89d4a50ccf71cf29ull, 0.60f, 0.10f, false, false},   // 18 blue crystal tiles / swirl
    {0xeb7b6494ca22395eull, 0.60f, 0.10f, false, false},   // 28 blue crystal tiles / swirl
    {0x25ec144f27f79ca7ull, 0.50f, 0.10f, false, false},   // 6 crystal
    {0x444628202540e35bull, 0.50f, 0.10f, false, false},   // 12 crystal
    {0x5dabeaab73c17563ull, 0.50f, 0.10f, false, false},   // 16 crystal
    {0xe08f5a4fddfed630ull, 0.50f, 0.10f, false, false},   // 26 crystal
    {0xe68e44c46da893faull, 0.50f, 0.10f, false, false},   // 27 crystal
    {0x67cc099e23143907ull, 0.40f, 0.10f, false, false},   // 17 crystal panels
    {0xf196a43734649fafull, 0.40f, 0.10f, false, false},   // 29 crystal panels
    // map/for00
    {0x6b95c5e48e54de8eull, 0.60f, 0.10f, false, false},   // 7 crystal spires
    {0x7e8b11d48828243bull, 0.60f, 0.10f, false, false},   // 8 crystal spires
    {0x806052ec2c403c7eull, 0.60f, 0.10f, false, false},   // 9 crystal spires
    {0xcbf4762859b4795dull, 0.60f, 0.10f, false, false},   // 17 crystal spires
    {0xe44c7b24be6a4a4bull, 0.60f, 0.10f, false, false},   // 20 crystal spires
    // map/nin00
    {0x2110fd3b196474f6ull, 0.50f, 0.10f, false, false},   // 2 copper crystal facets
    {0x2c429cf59998ec39ull, 0.50f, 0.10f, false, false},   // 3 copper crystal facets
    {0x4574e1d51bf48a33ull, 0.50f, 0.10f, false, false},   // 8 copper crystal facets
    {0x47f46b3791537dc5ull, 0.50f, 0.10f, false, false},   // 9 copper crystal facets
    {0x5de452fd6f2d3b30ull, 0.50f, 0.10f, false, false},   // 10 copper crystal facets
    {0x747433f4cfbe9101ull, 0.50f, 0.10f, false, false},   // 12 copper crystal facets
    {0x8c98ff1f57575641ull, 0.50f, 0.10f, false, false},   // 14 copper crystal facets
    {0xb8715c9042d2bfefull, 0.50f, 0.10f, false, false},   // 16 copper crystal facets
    {0xbc8e7630901913acull, 0.50f, 0.10f, false, false},   // 18 copper crystal facets
    {0xc091b154ef0a01d3ull, 0.50f, 0.10f, false, false},   // 19 copper crystal facets
    {0xc74afdd6f9c00af4ull, 0.50f, 0.10f, false, false},   // 22 copper crystal facets
    {0xf7e3ad9982f41964ull, 0.50f, 0.10f, false, false},   // 26 copper crystal facets
    {0x66ea282e268c29fcull, 0.60f, 0.10f, false, false},   // 11 purple crystal
    {0xc5beac95b2c23cecull, 0.50f, 0.10f, false, false},   // 21 grey crystal
    {0xdfb8e816134e716cull, 0.40f, 0.10f, false, false},   // 24 crystal
    {0x7f50e139b1b59952ull, 0.30f, 0.50f, false, false},   // 13 polished stone
    {0xcf9651d651a01a2dull, 0.30f, 0.50f, false, false},   // 23 polished stone
    {0xe094a5d34ca38f3full, 0.30f, 0.50f, false, false},   // 25 polished stone
    {0xf9fbbcc3250c1bddull, 0.30f, 0.50f, false, false},   // 27 polished stone
    // map/ten00
    {0x0ccb68e1810e9493ull, 0.35f, 0.30f, false, false},   // 2 marble floor
    {0xbeae3b3cdc33c754ull, 0.30f, 0.30f, false, false},   // 24 marble
    {0xf9a4dbe8ab204c56ull, 0.20f, 0.50f, false, false},   // 31 stone
    // map/thn00
    {0xbcd2789458ed0783ull, 0.60f, 0.30f, false, false},   // 27 marble inlay floor
    {0x44548d3da6c87a2cull, 0.50f, 0.30f, false, false},   // 13 marble
    {0x4d3a9cd7b21284caull, 0.50f, 0.30f, false, false},   // 14 marble
    {0x81d45b8691fd29ccull, 0.50f, 0.30f, false, false},   // 19 marble
    {0x96d150e9ec136d96ull, 0.50f, 0.30f, false, false},   // 22 marble
    {0x0d0445e23628dfdfull, 0.40f, 0.45f, false, false},   // 2 granite
    {0x3657785eaf4aac2cull, 0.40f, 0.45f, false, false},   // 11 granite
    {0x0097b1da884f6a51ull, 0.30f, 0.45f, false, false},   // 0 white panels
    {0x6abe6c46ece1c303ull, 0.30f, 0.45f, false, false},   // 18 white panels
    {0xd23b6ed37300e7faull, 0.30f, 0.45f, false, false},   // 30 white panels
    {0xda8d508c364eaaf0ull, 0.30f, 0.45f, false, false},   // 32 white panels
    {0xb95dde5bc7442e74ull, 0.30f, 0.55f, true, false},   // 26 metal
    // map/one00
    {0xa92cfe73368ea55aull, 0.50f, 0.30f, false, false},   // 22 purple marble
    {0x44950d6546c5dabcull, 0.40f, 0.30f, false, false},   // 11 polished tiles / marble
    {0x8c54fa5adb239dd5ull, 0.40f, 0.30f, false, false},   // 16 polished tiles / marble
    {0x977d1993764c3b3full, 0.40f, 0.30f, false, false},   // 18 polished tiles / marble
    {0xa1ba4e33471af5f9ull, 0.40f, 0.30f, false, false},   // 21 polished tiles / marble
    {0xe9a1672dcb6eced8ull, 0.40f, 0.30f, false, false},   // 30 polished tiles / marble
    {0x2de26e62083a11b6ull, 0.30f, 0.50f, false, false},   // 8 polished stone
    {0x57795f5123bdb18cull, 0.30f, 0.50f, false, false},   // 14 polished stone
    {0xd09c86253dcf6422ull, 0.30f, 0.50f, false, false},   // 25 polished stone
    {0x16af30d15d9277b1ull, 0.30f, 0.40f, true, false},   // 1 gold trim
    {0x26e47f20e5010e99ull, 0.30f, 0.40f, true, false},   // 4 gold trim
    {0x424ce5b853baa512ull, 0.30f, 0.40f, true, false},   // 9 gold trim
    {0x539660184f62b100ull, 0.30f, 0.40f, true, false},   // 12 gold trim
    {0x92ba9a70402b37cbull, 0.30f, 0.40f, true, false},   // 17 gold trim
    {0xca794ba6afda7d77ull, 0.30f, 0.40f, true, false},   // 24 gold trim
    {0xec3f27bc06a75fc4ull, 0.30f, 0.40f, true, false},   // 31 gold trim
    {0xfbbdae4ec6bb67fdull, 0.30f, 0.40f, true, false},   // 33 gold trim
    // map/vsi00
    {0xfdb980e4c6915bedull, 0.60f, 0.40f, false, false},   // 21 blue tiles
    {0xef2adb0561e271c1ull, 0.50f, 0.30f, false, false},   // 20 green marble
    {0x177c4f4dd13057c5ull, 0.30f, 0.60f, false, false},   // 4 polished wood floor
    // map/vth00
    {0xeeaef7e1d874ae04ull, 0.70f, 0.30f, false, true},   // 26 waterfall
    {0xb848be58e041e094ull, 0.50f, 0.15f, false, false},   // 22 ice
    // map/vtw00
    {0x103c3babfcbdf561ull, 0.85f, 0.05f, false, false},   // 0 glass floor panels
    {0xaae8daa33aff4b63ull, 0.85f, 0.05f, false, false},   // 10 glass floor panels
    {0xbae420dab239f808ull, 0.85f, 0.05f, false, false},   // 13 glass floor panels
    {0xf963b3175ce3c7feull, 0.85f, 0.05f, false, false},   // 15 glass floor panels
    {0x42f11048c387c48eull, 0.50f, 0.15f, false, false},   // 6 ice
    {0x7bebc0e43abfbb5bull, 0.50f, 0.15f, false, false},   // 8 ice
    {0x243354c957222227ull, 0.40f, 0.05f, false, false},   // 1 glass
    {0x38e1e67061e54424ull, 0.30f, 0.55f, true, false},   // 3 metal
    {0xd321cf89a8bfea6dull, 0.30f, 0.55f, true, false},   // 14 metal
    // map/ele00
    {0x2ad3aa78d7585cf2ull, 0.50f, 0.15f, false, false},   // 0 cloud layer over the planet floor
    {0x7223164cca530295ull, 0.50f, 0.15f, false, false},   // 8 cloud layer over the planet floor
    {0x3892e8ce7efc8bf2ull, 0.50f, 0.15f, false, false},   // 2 planet surface floor
    {0x3934c5b0b72bf573ull, 0.50f, 0.15f, false, false},   // 3 planet surface floor
    {0x4d487498a0c6393dull, 0.50f, 0.15f, false, false},   // 5 planet surface floor
    {0xa863771f164d0f16ull, 0.50f, 0.15f, false, false},   // 10 planet surface floor
    {0xa8c32809805d8980ull, 0.50f, 0.15f, false, false},   // 11 planet surface floor
    {0xa9d60889c7876de4ull, 0.50f, 0.15f, false, false},   // 12 planet surface floor
    {0xb3a8ee83358b4710ull, 0.50f, 0.15f, false, false},   // 13 planet surface floor
    {0xb519307ced2ba850ull, 0.50f, 0.15f, false, false},   // 14 planet surface floor
    {0xbab102db4a193f12ull, 0.50f, 0.15f, false, false},   // 16 planet surface floor
    {0xc2607db59ab010a2ull, 0.50f, 0.15f, false, false},   // 17 planet surface floor
    {0xcb64db3df3b90cbbull, 0.50f, 0.15f, false, false},   // 18 planet surface floor
    {0xd6caefea7fe5ad28ull, 0.50f, 0.15f, false, false},   // 19 planet surface floor
    {0xe34d9570b736aa5bull, 0.50f, 0.15f, false, false},   // 20 planet surface floor
    {0xe78645f79d07433dull, 0.50f, 0.15f, false, false},   // 21 planet surface floor
    {0x6373135b0f44ee59ull, 0.60f, 0.10f, false, false},   // 6 hanging crystal
};
}  // namespace

void dissidia_register_reflectivity() { psp_refl_set_builtin(k_refl, std::size(k_refl)); }
