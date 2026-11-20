#pragma once
#include "platform.hpp"
#include "thumb_png.hpp"
#include <cstring>

// Immutable platform navigation and checkbox skins from .2130 resources.
// Cairo PNG loading rounds premultiplication; the observed 1C resource loader
// floors it. Both exact resource-derived representations are pinned below.
// Unknown images, including arbitrary user pictures, always pass through.
namespace enterprise_navigation {
using PatternCreate = void* (*)(void*);
inline thumb_png::CairoApi api;
namespace detail {
enum class Skin { navigation, subsystem, taskFilter, selectedTaskFilter, taskFilterEdge };
struct Asset { int width, height; const char* digest; bool checkbox{}; Skin skin{}; };
inline constexpr Asset assets[] = {
#include "enterprise_control_tiles.inc"
    {17, 17, "a76c6efa0862072b93330b6f2dba62465bfd12a219c0f8d154fe1f24575b4014", false, Skin::taskFilter},
    {17, 17, "f1f148b811ac2a2087e56a89d30902d9597557366499afa50d0592c5d0f764ac", false, Skin::selectedTaskFilter},
    // Borders and corners sampled from the stock task-filter drawing route.
    {17, 3, "31af59a781fa2fc3a3330512944b326277174659217c550ca9e82f72c2da763c", false, Skin::taskFilterEdge},
    {17, 3, "cf2a16aff1d8693de663de1924dc22572ef4d8660108e72e34dca0c97a3fe07b", false, Skin::taskFilterEdge},
    {17, 3, "e726e9b6c770a232bcdf2726fabfa41ac3e791512c1b112daf7950b74c72b755", false, Skin::taskFilterEdge},
    {17, 3, "ede71ae7e5e5c76ff67f4650057d527b9a88e4680b9200edc804eb879892a88d", false, Skin::taskFilterEdge},
    {17, 3, "f360dde4ef2ce1cf2eb1c43fc8cb431512048807852bb621ae901362e5c619b4", false, Skin::taskFilterEdge},
    {3, 17, "0e090cd9bcf7941b49d32cd99c32bbfd1b72b821995951a3be8faf19fdd84add", false, Skin::taskFilterEdge},
    {3, 17, "53d3c4ed963946d49fea9f9d702ce31f3051c252d5f9df05b755b1fac284cd6c", false, Skin::taskFilterEdge},
    {3, 17, "905678d246891970e8674d02330623f0c11794bed333d994b6ee75caff7f4dbf", false, Skin::taskFilterEdge},
    {3, 17, "96543a6b129d9f58a10fa648ba9bc85e0cb08551119664cff84f5417a38112f0", false, Skin::taskFilterEdge},
    {3, 17, "c54d1d7ab9b162e54481b04c84690ff93d56c00fb6829cd95d2e84f09858d63a", false, Skin::taskFilterEdge},
    {3, 17, "cc6f8d9305d52ade3cbd0e2e0dea6964b0519c2e71b34d0589d01cfbf0a78a4f", false, Skin::taskFilterEdge},
    {3, 3, "07aaf4e0483881e7a36d0920276276725a958e744e2372c9182bc6ac300e53a8", false, Skin::taskFilterEdge},
    {3, 3, "1329caa2245d1309bb4f2d03bffd006fe81089a1ae939286ec76743a5e0c4a46", false, Skin::taskFilterEdge},
    {3, 3, "2c1a26294c1b0aa19a09e19c6cfc9f37fbb36e6f9a4f588cab2c30000a379b67", false, Skin::taskFilterEdge},
    {3, 3, "3dad53b7b2692ba3f94fb4bd548b9be9abe9d46dcf2f6986d67866fe2eef35b4", false, Skin::taskFilterEdge},
    {3, 3, "470992609bdba75831fb53e11e9d2862eae099dd51ca52fa6af3940bd434be4f", false, Skin::taskFilterEdge},
    {3, 3, "5541b56e81c403d31768943bdb0c4fca4647972b63663ca4b946de761db3fc1c", false, Skin::taskFilterEdge},
    {3, 3, "6fc83689398bba95962d341acd02f37093704f53c071928c9f048e82dc53ef91", false, Skin::taskFilterEdge},
    {3, 3, "78bacd238b7c6c50937238c9b5c392ad401998a1cc64f271ae0b61b7237bf4ef", false, Skin::taskFilterEdge},
    {3, 3, "7c031da396d322caed9ce1984f18f11c9444ff39b1f5201c5516e19c54173728", false, Skin::taskFilterEdge},
    {3, 3, "7c769ce8bb90cab3ecb324ac59fadbf022eb4d97425341d550bd3d1ba5223841", false, Skin::taskFilterEdge},
    {3, 3, "88331e33f9e26748385a0d3b43623824866bb44e229fc8bf5a25eae0f5915d90", false, Skin::taskFilterEdge},
    {3, 3, "8ec5c4f1e4377417fdd02e9ed079fd35a51baa6ca835c6b4e060df0b81f36afe", false, Skin::taskFilterEdge},
    {3, 3, "be2f26663fe567e9d1b869e02dc92269b40884f0238e3db38ca81bd1f239ad42", false, Skin::taskFilterEdge},
    {3, 3, "dc70305ff5a299c273f9fa166264cbef0d654694a1fc307cfdfedcd5816e3e94", false, Skin::taskFilterEdge},
    {3, 3, "dffb7cf6e7601d8f396b982c378dca3056f847e28deb22c0c7e3cf2b4b3b60ef", false, Skin::taskFilterEdge},
    {32, 32, "e565f2ae52d3252ec73a679ca77b4fb19e81e33c2b9ccc82c41be2970c2aba91", false, Skin::subsystem}, // defTopSubsystem_100.png rounded
    {32, 32, "8948aa05a2a76e8d17a9ec4e53dc219e75e43c160305af303dbe5c3d7c78cf0f", false, Skin::subsystem}, // defTopSubsystem_100.png floor
    {40, 40, "b1cd65d1cb4a918710a2d385c3a47e8d84e567e5500018d5fbfa80e17ea02d47", false, Skin::subsystem}, // defTopSubsystem_125.png rounded
    {40, 40, "15f5d336344addf4a4158c51c5a8f66cd7bda6cb1e376b96b3da318ddd87f3e2", false, Skin::subsystem}, // defTopSubsystem_125.png floor
    {48, 48, "4135f614b6820293eed47b730169fe78428124f883a6f195246d9317cc4e5838", false, Skin::subsystem}, // defTopSubsystem_150.png rounded
    {48, 48, "38b98a9bb445524a8c76506fd9bd44b60863616dde02580de0825584968918ad", false, Skin::subsystem}, // defTopSubsystem_150.png floor
    {16, 16, "918ad96fb8b7dc7959ea52cb700ce9a3f9fd43527bc6510a2fb3caca34bf18cf", false, Skin::subsystem}, // defTopSubsystem_16_100.png rounded
    {16, 16, "d60f17a9b6520a487e305515d92b41d1af526f6bfdb39750e4f922cfaba79311", false, Skin::subsystem}, // defTopSubsystem_16_100.png floor
    {20, 20, "01ff1802873c5ba52ab6e1e191364c84d10978bce9477adb7c1936e623e50a13", false, Skin::subsystem}, // defTopSubsystem_16_125.png rounded
    {20, 20, "c98c428bc80a8d9890346c246e2a0a1a65aa929ac0be0a84a86547472f6764c3", false, Skin::subsystem}, // defTopSubsystem_16_125.png floor
    {24, 24, "fb12690433597fef1aab1dfe697ed7ee112bb327540303905ea79072ef5208d6", false, Skin::subsystem}, // defTopSubsystem_16_150.png rounded
    {24, 24, "39917757d95265dbbb9875764aa907477afc4759e01edb5a601a43a2859ef636", false, Skin::subsystem}, // defTopSubsystem_16_150.png floor
    {28, 28, "b510369cdcdaaacabddfb11d9017870211db5805a24737d5ba78012b1a2fa8ab", false, Skin::subsystem}, // defTopSubsystem_16_175.png rounded
    {28, 28, "247c75fbd7b2580f8d67490da8f5f50a136994a032c398b98a5bb2a2b6de8f27", false, Skin::subsystem}, // defTopSubsystem_16_175.png floor
    {32, 32, "b9b6c1f886c55173ef093c8e6918bf6ff1b5282dfbad3e56a9ec46a7c4b63450", false, Skin::subsystem}, // defTopSubsystem_16_200.png rounded
    {32, 32, "53b69363b9d8284f1a3d5a6793de1ef393af9cf4c4ead9f5add0a6adc7c12d07", false, Skin::subsystem}, // defTopSubsystem_16_200.png floor
    {48, 48, "522830bccbee9ff47505ed3c353d03a161c8ca799baaced0e93d7bc8e0c70e4f", false, Skin::subsystem}, // defTopSubsystem_16_300.png rounded
    {48, 48, "8d738703e4663d6259239df5fa7b74306a9d93a01f059e8f37a5cb5e77afef7d", false, Skin::subsystem}, // defTopSubsystem_16_300.png floor
    {64, 64, "12217c9c75f334cfc25c3c64740318749d8fe1e1c8f4f78b93d221bac571c78d", false, Skin::subsystem}, // defTopSubsystem_16_400.png rounded
    {64, 64, "a6cf642891d45a3494dbe68665c0a151ff14512cc78a47b2cb644fabbd717369", false, Skin::subsystem}, // defTopSubsystem_16_400.png floor
    {14, 14, "ec516cfb1cced7f84a6e4bb84781d3b347694ef4aea041b8c0b4075ab2a9d057", false, Skin::subsystem}, // defTopSubsystem_16_85.png rounded
    {14, 14, "9b9efd0db4a327aa2a2b7c6a6c86ee5f0ad5c4881c984b1e21a7959e44d77d22", false, Skin::subsystem}, // defTopSubsystem_16_85.png floor
    {56, 56, "80630aaf4289f8e7f1524f137d498cd0e47a1238e820bf4c9c864170c759eb86", false, Skin::subsystem}, // defTopSubsystem_175.png rounded
    {56, 56, "c2ff5074a26b658daf40f8f0d677000289bafd33274fc73ad73c243f54dc5911", false, Skin::subsystem}, // defTopSubsystem_175.png floor
    {64, 64, "0dc94cc5969e7d6941b6bcadc418e600cc23efcab6a574acafba9aa09a8880c0", false, Skin::subsystem}, // defTopSubsystem_200.png rounded
    {64, 64, "5721e1a4e164f2a8b29da26411226560fe0e5b8861a8b4a49c5462c06179930a", false, Skin::subsystem}, // defTopSubsystem_200.png floor
    {96, 96, "6688ee9a695ce04e6ece564bb3839fa7d563c9a6b2ce8b437ea9cb56d4d65402", false, Skin::subsystem}, // defTopSubsystem_300.png rounded
    {96, 96, "4b8f71b852059569048271a8da29cf26a3663f2af64abf140fe8a49de2fda519", false, Skin::subsystem}, // defTopSubsystem_300.png floor
    {128, 128, "d2a9c8178a5fe24bfbfa7fc4eb532e3b3f916f08b0baf98193af443c2bf73a49", false, Skin::subsystem}, // defTopSubsystem_400.png rounded
    {128, 128, "4a35a9b67da8ed88f7a2ed59dc7ffee6ac0b8e4bafd0a4a5571c1d0ffc235899", false, Skin::subsystem}, // defTopSubsystem_400.png floor
    {28, 28, "33fa811149e593a5e671493b4035868888423d69c8cceb03e6bc3d21d6555b94", false, Skin::subsystem}, // defTopSubsystem_85.png rounded
    {28, 28, "81e4d7f18aedede3ef0ff46301a6901543dbe1057d77fa7003b7f85e41aa9d0f", false, Skin::subsystem}, // defTopSubsystem_85.png floor
    {38, 25, "7908beed1902b1e46a2a1a6fd2c17575ac1078e4b025a067c70c8d126e142000"}, // tlBackDisabled_100.png rounded
    {38, 25, "66dded0ccda0472e4f2459547a9df6398d8c4cdb74777c967fe3b4bf926bbf18"}, // tlBackDisabled_100.png floor
    {47, 31, "bfb510d505526191570fd4dff5317181780ba5b1de3f403d95dce288e6f28917"}, // tlBackDisabled_125.png rounded
    {47, 31, "598f151826615ed2f2821aee4abe1e53bbc32e0d30631d83d7b84e7de131553e"}, // tlBackDisabled_125.png floor
    {58, 38, "b8568ac514c04ede26c2ad54641eafbbc2012ad50661617b621c900b7064160e"}, // tlBackDisabled_150.png rounded
    {58, 38, "987fc122cb4b27fdd6145388371344651f711aef17e54fd1433e9d04c7b1cafd"}, // tlBackDisabled_150.png floor
    {67, 44, "f44e7fa6626ab7ab2903f14aa358536bd47c2d806895b0ee99b35e9781228324"}, // tlBackDisabled_175.png rounded
    {67, 44, "40bc57e738e055de0fc558d4f09be8ece81e798a26dca8adca11fa6eeedcd15d"}, // tlBackDisabled_175.png floor
    {76, 50, "436553531bd84c237546b6579b6fc46f9400a35321d06dcc8aa09d3d4bfd8d04"}, // tlBackDisabled_200.png rounded
    {76, 50, "b4b53639aaf29b2356bcd4938ff5099d8e3b47d2bb0b967ad96257c675e06282"}, // tlBackDisabled_200.png floor
    {114, 75, "f8936d289e4ca0071352e9e7cc168e75eae81016a312cee9be9213a664218e12"}, // tlBackDisabled_300.png rounded
    {114, 75, "27d340c907549b414499fb0a1aa5f4f9e86231ae589003e3f9f1545cdd14c8ca"}, // tlBackDisabled_300.png floor
    {152, 100, "a5dcc1772e399b5cde11c6338dcf1c46ce05612f1ed9ecd7937c1bad0936a62d"}, // tlBackDisabled_400.png rounded
    {152, 100, "de0893e660feff57559b4101a825418accd8036228caba298363965615f5c5cf"}, // tlBackDisabled_400.png floor
    {32, 21, "366acfd60381b27998d785f37d9506824e3d84e0be3ba69a84e42b34c6a724d0"}, // tlBackDisabled_85.png rounded
    {32, 21, "95e84dbafb4e1290ae7e9daf30e807d27d5ff45b12782e26f675dff7ca39d1df"}, // tlBackDisabled_85.png floor
    {38, 25, "6c224f86c1dab03501376a5e744dc28ece63018c2b44d90b08e48de9ba1251c0"}, // tlBack_100.png rounded
    {38, 25, "cd839a613c99e3d670e287f19fe85f36cc68038faf2531f5009319e0b8a9d9c5"}, // tlBack_100.png floor
    {47, 31, "40840fcc0bc8e73dfe8bdf232eb4a0f1628ac32fca95f112f9c21abb47ff59f1"}, // tlBack_125.png rounded
    {47, 31, "4490257756ae9ba588954c9bc3bf08be1a2e08756ce86639a75c7dc1e28955b7"}, // tlBack_125.png floor
    {58, 38, "b05e2c15a2dc8e4b710453ecec2d7416f1ea98218150cd11e5924cb52db4b5d2"}, // tlBack_150.png rounded
    {58, 38, "3e44548f8c3473bfbd4ba7f4d900d2da01fc3670a0c5e47f2a6d6cf8136542b4"}, // tlBack_150.png floor
    {67, 44, "842fe7c595a2762ee363648de45f296809f2aea4db011534c4bafccc7488ee8b"}, // tlBack_175.png rounded
    {67, 44, "82c0f08a48394178a966e192c177764816851980e4d36ff6e146c9c92cefed16"}, // tlBack_175.png floor
    {76, 50, "c61cd5bd73439381ffc6da1fdc8e24c355e102619a322898c1f0fbb23796b2e8"}, // tlBack_200.png rounded
    {76, 50, "808869a3cc3ec88db7d7a117239524ec8a622c49b3314615ce0019026891c077"}, // tlBack_200.png floor
    {114, 75, "2d97248cdc9a996fd7eb929e47254cd8a697b58b3188fb72880d0c730fbfb3d9"}, // tlBack_300.png rounded
    {114, 75, "537ed641015bf443d7a63c6f9029fc6b0cd3c5782ac4fe28e7ab546d2f2f2af5"}, // tlBack_300.png floor
    {152, 100, "336d45e98a66ff0f28731aaf3e779859ddf0ff915e84cef27b15650cb73cd635"}, // tlBack_400.png rounded
    {152, 100, "d8b80486f3f72eece96821145edace1c4bf0449f8750add3a4710819c8df1cb6"}, // tlBack_400.png floor
    {32, 21, "363f9c874da1400277ba47b615fb2e0dda94b62bc093b190da2ce4df19f267cf"}, // tlBack_85.png rounded
    {32, 21, "6d917bf57ef10c80b68ebd551e330defe547945e572aae87f2e097407e040444"}, // tlBack_85.png floor
    {36, 25, "3aa5f09f30242aaee18dc2a8fd264d102d5a740fe3c5ad9d865e68ae6856f863"}, // tlFwdDisabled_100.png rounded
    {36, 25, "d4bede7273084e1e7d864e51dad9f965363b40f9f7caba0965db477ff5fcfe6e"}, // tlFwdDisabled_100.png floor
    {45, 31, "dcb31778e9f8a3d4c93f61e4b0ee18d395f6510227276488502abc7e238b1f2b"}, // tlFwdDisabled_125.png rounded
    {45, 31, "0c20b4ac1af4b22ca9088773efa3525c0bb63be9d142c6e63a3008bffba557b4"}, // tlFwdDisabled_125.png floor
    {54, 38, "26dd381bcef6813d43968b3914da25c0902855c0fb7507222c9f2b493d7ce3fa"}, // tlFwdDisabled_150.png rounded
    {54, 38, "baa402bc68bc8af218319b83baaa0fd71658887aae25f0522d0b4665c33fa02c"}, // tlFwdDisabled_150.png floor
    {63, 44, "7d4d2706f9ce11afb69e052ec291d6933335aaf1433c22886b2e55925baeff21"}, // tlFwdDisabled_175.png rounded
    {63, 44, "5e3e4648464a539b068d77a386130ee8fbfd0025fe68494bf11740704b59d485"}, // tlFwdDisabled_175.png floor
    {72, 50, "48c54c857bce98715986deeb5ff5431758333a9241f792290796805d740db129"}, // tlFwdDisabled_200.png rounded
    {72, 50, "fbba96f9ce236070bdaabdad9155f258bdcdf6b2b316a57a4a884b8bf983539c"}, // tlFwdDisabled_200.png floor
    {108, 75, "efa897070753afc16007fef5e138dedfaaa2035713158e4e907b8e69e72254e7"}, // tlFwdDisabled_300.png rounded
    {108, 75, "5d89ab1155b07d02ad3ec352c8047e29ee2bf3bdcf4085229711f5dccd7aea1a"}, // tlFwdDisabled_300.png floor
    {144, 100, "4f44721536164495a1ef22eedd95655955bed6f054f7e62431708d97aa7ed062"}, // tlFwdDisabled_400.png rounded
    {144, 100, "e1466024b61808e079f2cc82bd8c56f42e22c44b8359106ab6d1175d59e41066"}, // tlFwdDisabled_400.png floor
    {31, 21, "f7d4c76d353ea1770f87f6108c23c75bccb7fea3b5b82f1eb5f62f2c605e5f55"}, // tlFwdDisabled_85.png rounded
    {31, 21, "3dbe3b6dc19a0645c737a1eab111ca770e21ab3bb8cd468d26058d9cf795ec28"}, // tlFwdDisabled_85.png floor
    {36, 25, "fa067d76570108115635e078f007a4fdab4de6245ae982afd7aa699e67da051c"}, // tlFwd_100.png rounded
    {36, 25, "8f5fd64056968b83913731ba8e6098409b64151de295abf6a85230e6fb0e7e75"}, // tlFwd_100.png floor
    {45, 31, "0e127479c3aed9c9fa17e22afe31eb4b7d5478d141c63e3a8ea5d873e3e34fda"}, // tlFwd_125.png rounded
    {45, 31, "b2f24708521818a9833a8b0f21123035d812e151de26e617a4fe89e870d0f205"}, // tlFwd_125.png floor
    {54, 38, "c45eccfc124875246b592ed3454103ac3e2caea3a1d74e5a238b63933db51bf7"}, // tlFwd_150.png rounded
    {54, 38, "1f7393d125362721614152c8774f7e1e02f8e6b0020ed662991d860cefe6c3f7"}, // tlFwd_150.png floor
    {63, 44, "12b653e4d511d1555428f14b426b9b8b040146906481a0b90153688b4bda933a"}, // tlFwd_175.png rounded
    {63, 44, "43f797e922b229baf06f78e15380a3c910ffaa86afdf184c7e988ee964096f7f"}, // tlFwd_175.png floor
    {72, 50, "5390289706760b1d9a6ac092e00d240c5879a942f9879269005e443c5fc2b63b"}, // tlFwd_200.png rounded
    {72, 50, "50e2729c4a718f273fa4f608e1df2bdb2dbf31e13d9443c0b969dbaf33ddbc47"}, // tlFwd_200.png floor
    {108, 75, "8a724018775104147a7cdf6365fc6dfb47a25ba95e1d8fd4e5972bce0dfa2638"}, // tlFwd_300.png rounded
    {108, 75, "0c91cf7e290429dfbee7f695c950b2c304db99ddb943a6ba6d9ed09695bf0386"}, // tlFwd_300.png floor
    {144, 100, "239b459ff42e89e29c27df2df18042d82b304661714dffa08b4e0a3720b4870a"}, // tlFwd_400.png rounded
    {144, 100, "96bfe1df330b93028fc8a46e4e8e65797bad6b6e7cb6fd54e823d43c836d17d3"}, // tlFwd_400.png floor
    {31, 21, "72c87e2bfac92bb9d669b79fddcc8bb421537f4381662c548ff9d1a483b4e9d6"}, // tlFwd_85.png rounded
    {31, 21, "13aae6ba37f5a0f7c9d481c8885eb52a27af04d2c4a6bad6a83c977d0dc7eace"}, // tlFwd_85.png floor
    {16, 16, "4af317ac7ba32867f39021ec20fbfa94e6d1ecbc70d37d1560b4f93a915f8da6", true}, // checked_100.png rounded
    {16, 16, "224da71ba053003f0169ad427d411529027654321639aee7f70bf1f1f8db35b2", true}, // checked_100.png floor
    {20, 20, "40a82da6ef5437460493a05b00205b1b03192b61207e875f517063880589a4e2", true}, // checked_125.png rounded
    {20, 20, "301d450bf60b458b5ee9f0307c70f0537280dbd4fc7d13bdfb70a98a6d8746d2", true}, // checked_125.png floor
    {24, 24, "ee9a667e549e32b52386fce3f1ea3d0bde61efbf61f7cf1a0df2d6cf6f09723b", true}, // checked_150.png rounded
    {24, 24, "915f929a016dcaf614ad51356c7c064fa7ca2b35b58c9dae2214468f7c75913c", true}, // checked_150.png floor
    {28, 28, "a33e367f28fedf517137a9a04d5f0050ceffecbcd7750686eb3014f70dfcf16e", true}, // checked_175.png rounded
    {28, 28, "608a5ed3b51273318e39462286fe9537ebb88939b8d76bab5d74d7189d2e548c", true}, // checked_175.png floor
    {32, 32, "a24a36cb54e0efa610ab7d64765f70f022ff4858126adad06b592155fb59c26d", true}, // checked_200.png rounded
    {32, 32, "0c9ff497c01f782c736f5ff3bb3894e38977cdd27ee66d53eacab7e2410bcd80", true}, // checked_200.png floor
    {48, 48, "1e75fc9a9502dcc76989471d3d65c823273c3141be8e6ae5f4540dfc75b1cfdb", true}, // checked_300.png rounded
    {48, 48, "979624f7a3954f4851f261d17c8bdc26d98e9dd00ab364cb8a87f2fd312f2a55", true}, // checked_300.png floor
    {64, 64, "e63651ab86c53bd44a6b85bc6ecb6badd96fe3ac1e0c9b8a3084a967cc8d5654", true}, // checked_400.png rounded
    {64, 64, "2c8a6241b49fe1137d1c85315c79d17d693d7c7fd94506c4fcd90583ad017918", true}, // checked_400.png floor
    {14, 14, "b5839185905b50e98d289447ed2759c1871957a777c6d9276aabef34d06a81a4", true}, // checked_85.png rounded
    {14, 14, "98be76ce6dec203e2c3560dd1411e2127c5d968058b850f1a7abfd883dd5891c", true}, // checked_85.png floor
    {16, 16, "58e840c5bc234748cac6212549d43ebfd4a3f20acebaa0a473724e089172cb14", true}, // discheck_100.png rounded
    {16, 16, "42234e56f555f060ae31c2fd2f2f5bec14c74aa85ca075059f2eb13bb1553b04", true}, // discheck_100.png floor
    {20, 20, "f842fdd0bd543967dd9842843516da027257e8fb4b89290ba470838fa220a98d", true}, // discheck_125.png rounded
    {20, 20, "36c87df8ce91c8efc499854ea6749716392af161bc74770c9074dc993646d276", true}, // discheck_125.png floor
    {24, 24, "bc253ec8b55a3ba3b6c38246fe32d59ce3ba2888f882a6be54ed1c81e44fd35e", true}, // discheck_150.png rounded
    {24, 24, "72581d5482d8503eb56a70b7bf048a0d52122bb15f706ffe0832c84de26996d3", true}, // discheck_150.png floor
    {28, 28, "b6b333b289419984b7fc7e494e95fd63b0b80893ec4d9dd8eb48956901760d50", true}, // discheck_175.png rounded
    {28, 28, "9a7f1b5171e2fac9f4e523d19081eb58bdf36206bd31576000ec383cfdb9ccb4", true}, // discheck_175.png floor
    {32, 32, "fda2dc62c6292356f54d704b7436cd3938dd7b408427082dd500491f5d525e0c", true}, // discheck_200.png rounded
    {32, 32, "33be47fe76f8af45e5e555ecf73131870fc0c76459f1500abde776940fc6eb3f", true}, // discheck_200.png floor
    {48, 48, "7e4fa51058476324104d6de9a3934a7603af780b039630c3d0aabe9cc1429983", true}, // discheck_300.png rounded
    {48, 48, "14dc30dd8b0e02f043be88e74a1776a9a2b183130a8ecfe90ea6f0e6dab3ac7a", true}, // discheck_300.png floor
    {64, 64, "29dcaf05d77c2827f03586cd6b5ff34da822e6d49636ee175628ed8c9d7c1a5d", true}, // discheck_400.png rounded
    {64, 64, "3e5fb6eb31da6c1176e4ef552db5403b76df8511277a226d0c13b9ff9da30d5f", true}, // discheck_400.png floor
    {14, 14, "95c4dca4237b3f1a902b9902646bf3b8bc9fe5a646fb93bc2e1aacd9c774100e", true}, // discheck_85.png rounded
    {14, 14, "0a01979d3760ca2e7c508cf92ade1e0e9e2adcd057b8eadf2b8f34083692ca93", true}, // discheck_85.png floor
    {16, 16, "b1c7dbec3a3f401eff0faaaf4aa4202d4fb9ab59d53f01b8bea3bc2bebd43ab6", true}, // unchecked_100.png rounded
    {16, 16, "bcfa31e73a5684c322cb980f748796d2f95cbfef262e58eda5bef73424949c0a", true}, // unchecked_100.png floor
    {20, 20, "57ca587dc788bd75ce87216b4c805a26c9fe4f7e04ceb81f5a71f1d598617b00", true}, // unchecked_125.png rounded
    {20, 20, "cee51e89919146f3be9f27c82d20399a4d56cf45a78eff972b23a28ee1fc61cd", true}, // unchecked_125.png floor
    {24, 24, "2f5f564ddfdb507623a6620d370f0bbf1bc265a718402f564378a7f755dddc87", true}, // unchecked_150.png rounded
    {24, 24, "885e5330edc6f13d3b881352cd60e9e48e883858b40310b03f4bc712071c3c8a", true}, // unchecked_150.png floor
    {28, 28, "f6b6edd1448606cea5d38f36492715a84737c29522ae4a74a6b0e79656dcea9e", true}, // unchecked_175.png rounded
    {28, 28, "c894cb43d4ee8d76b90d377ac146dc25287827e0f78bde6c251c98406510d832", true}, // unchecked_175.png floor
    {32, 32, "47dc2cfd0385eacb0a2d0cac3a3dc674ddcf8ff7a4ee9c0d71a9ad651e93a09a", true}, // unchecked_200.png rounded
    {32, 32, "ac8ca720690e9ade2cd6a1a36127d75c7985a148fd46ecaf077027bc701ed53e", true}, // unchecked_200.png floor
    {48, 48, "4c35c5bdd04012e29a57058ff5caf35a1c01a196cf6f88c36957012e5f6abccd", true}, // unchecked_300.png rounded
    {48, 48, "439698cc642859fa59d206ca80ce499edba06418bf513f69f61d1331972cecc8", true}, // unchecked_300.png floor
    {64, 64, "da2473f87aec917e533bc94748bdb527e81bf5296a136a43c77cd7b7548e0e0c", true}, // unchecked_400.png rounded
    {64, 64, "990d7c536c49b7b7d90de31632af863bb5dd47d9d3b0c205ce43c24e4e481c8f", true}, // unchecked_400.png floor
    {14, 14, "54c1dd62ce577d5c384135f814688beea652bfcb61a624acb9e09e589c884019", true}, // unchecked_85.png rounded
    {14, 14, "afe26060bd1fdcd7ac67cf8137993af8dad8c56346630a0154036ae07b2fdf77", true}, // unchecked_85.png floor
    {357, 17, "321c1c0a43ffe5fefa106dc68a30210434766c0da3ac49900033c6af7b4f4577", true}, // check_83l_100.png rounded
    {357, 17, "e7db849dc47e67503cbe8ae71b3439a5001ac12a3aa2eede403f19f51de829d8", true}, // check_83l_100.png floor
    {441, 21, "86d6cec2d48b302de9d086a6c05da0a9353b136b2af7b6bcb088388903777bef", true}, // check_83l_125.png rounded
    {441, 21, "6f73b1042b0ca5fc0d36f75882c1ee4bd8f67cfb1b9889906616e835af2b28e6", true}, // check_83l_125.png floor
    {546, 26, "51c1b5ebbb9f4d6d03d492451286b902ac5f48051646d5df0fe1a810da835e8b", true}, // check_83l_150.png rounded
    {546, 26, "67fca79b5d19067ac44403b1b798ac45b7160e80c2990713ad86b177d7f0bb5e", true}, // check_83l_150.png floor
    {630, 30, "515fea1c635f11cb90614dc9ebfc3ff6324a6837432fa5f81f05b8290d692eb8", true}, // check_83l_175.png rounded
    {630, 30, "11c31b496db568a4074038fa2f7f3e3def5402c0d38ec549687ddd6199a03c42", true}, // check_83l_175.png floor
    {714, 34, "ec8a8cc6c68db648ec0ae595ad4daf13b07da78025c38d71850fd620b66e3699", true}, // check_83l_200.png rounded
    {714, 34, "eccee542a1b4de9532cafc216d15dee4ece402b944cd6e61611e26cbf48e513e", true}, // check_83l_200.png floor
    {1071, 51, "b0d1c5c77daafcce8e29dcec4c831ec2409a7ff75d7e759adcf62cb60aaf0c8b", true}, // check_83l_300.png rounded
    {1071, 51, "9b5a9696f306045ca931a4bf545cc9923d439eff3ba4a462af6e0ca4902fb83c", true}, // check_83l_300.png floor
    {1428, 68, "0c7aeed18501495705e999178c441f09454d1c9d9798b0a1c4d82503fa9423f8", true}, // check_83l_400.png rounded
    {1428, 68, "4331dabfcee0c66a257419bfcf0d75cf1e4163b3b6400cb0562eda85aac02d10", true}, // check_83l_400.png floor
    {294, 14, "15605a755c5498e731a287e957e571459170af0f1f2e9eee09f5dd601378e825", true}, // check_83l_85.png rounded
    {294, 14, "3b93cf62693d6f2dad4511d425055c0c9c1f8de373c061e647fae1a11e65bf38", true}, // check_83l_85.png floor
};
inline const Asset* matchUnsafe(void* source)
{
    thumb_png::detail::Image image{};
    if (!source || !thumb_png::detail::complete(api) ||
        !thumb_png::detail::describe(api.surface, source, image, true)) return nullptr;
    bool dimensions{};
    for (const auto& asset : assets)
        if (asset.width == image.width && asset.height == image.height) dimensions = true;
    if (!dimensions) return nullptr;
    api.surface.flush(source);
    if (api.surface.status(source)) return nullptr;
    const auto* pixels = api.surface.data(source);
    if (!scroller_png::detail::pixelRange(pixels, image.bytes, false)) return nullptr;
    // Atlas ARGB32 is contiguous. For small surfaces with padding or RGB24,
    // canonicalize each row and omit RGB24's undefined high byte.
    unsigned char canonical[152 * 128 * 4];
    const bool direct = image.format == thumb_png::detail::argb32 && image.stride == image.width * 4;
    if (!direct && (image.width > 152 || image.height > 128)) return nullptr;
    size_t position{};
    if (!direct) for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x) {
            const auto* p = pixels + static_cast<size_t>(y) * image.stride + x * 4;
            const BYTE alpha = image.format == thumb_png::detail::argb32 ? p[3] : 255;
            if (p[0] > alpha || p[1] > alpha || p[2] > alpha) return nullptr;
            canonical[position++] = p[0]; canonical[position++] = p[1];
            canonical[position++] = p[2]; canonical[position++] = alpha;
        }
    unsigned char digest[32];
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, direct ? const_cast<BYTE*>(pixels) : canonical,
                   static_cast<ULONG>(direct ? image.bytes : position), digest, sizeof digest)) return nullptr;
    constexpr char hex[] = "0123456789abcdef";
    char text[65]{};
    for (size_t i = 0; i < 32; ++i) {
        text[2*i] = hex[digest[i] >> 4]; text[2*i+1] = hex[digest[i] & 15];
    }
    for (const auto& asset : assets)
        if (asset.width == image.width && asset.height == image.height && !std::strcmp(text, asset.digest)) return &asset;
    return nullptr;
}
} // namespace detail

// Initialize before publishing the hook; immutable afterwards. The caller pins
// Cairo and admits its platform hash before calling this function.
inline bool initialize(HMODULE cairo)
{
    if (!cairo) return false;
    thumb_png::CairoApi next{};
#define NAV_SYMBOL(member, name) next.member = reinterpret_cast<decltype(next.member)>(GetProcAddress(cairo, name))
    NAV_SYMBOL(surface.status, "cairo_surface_status");
    NAV_SYMBOL(surface.type, "cairo_surface_get_type");
    NAV_SYMBOL(surface.format, "cairo_image_surface_get_format");
    NAV_SYMBOL(surface.width, "cairo_image_surface_get_width");
    NAV_SYMBOL(surface.height, "cairo_image_surface_get_height");
    NAV_SYMBOL(surface.stride, "cairo_image_surface_get_stride");
    NAV_SYMBOL(surface.scale, "cairo_surface_get_device_scale");
    NAV_SYMBOL(surface.offset, "cairo_surface_get_device_offset");
    NAV_SYMBOL(surface.flush, "cairo_surface_flush");
    NAV_SYMBOL(surface.destroy, "cairo_surface_destroy");
    NAV_SYMBOL(surface.markDirty, "cairo_surface_mark_dirty");
    NAV_SYMBOL(surface.data, "cairo_image_surface_get_data");
    NAV_SYMBOL(surface.imageCreate, "cairo_image_surface_create");
    NAV_SYMBOL(setScale, "cairo_surface_set_device_scale");
    NAV_SYMBOL(setOffset, "cairo_surface_set_device_offset");
#undef NAV_SYMBOL
    if (!thumb_png::detail::complete(next)) return false;
    api = next;
    return true;
}
inline const detail::Asset* findAsset(void* source)
{
    __try { return detail::matchUnsafe(source); }
    __except (thumb_png::detail::exceptionFilter(GetExceptionCode())) { return nullptr; }
}
inline void* recolor(void* source, const ThemeProfile& profile, bool stretchedControl = false)
{
    const auto* asset=findAsset(source);
    if (!asset || (asset->skin==detail::Skin::taskFilterEdge && !stretchedControl)) return nullptr;
    const auto& colors=profile.colors;
    auto background=colors[barsBackground], edge=colors[asset->checkbox ? propertyMutedText : barsText];
    switch (asset->skin) {
    case detail::Skin::navigation: break;
    case detail::Skin::subsystem: background=colors[propertySelection]; break;
    case detail::Skin::taskFilter:
    case detail::Skin::taskFilterEdge: background=colors[propertyBackground]; edge=colors[panelBorder]; break;
    case detail::Skin::selectedTaskFilter: background=colors[propertySelection]; edge=colors[panelBorder]; break;
    }
    return thumb_png::clone(api,source,{background,edge,asset->checkbox});
}
inline bool matches(void* source) { return findAsset(source)!=nullptr; }
inline void* patternForSurface(PatternCreate original, void* source, const ThemeProfile& profile)
{
    const auto incomingError = GetLastError();
    void* copy=recolor(source,profile);
    SetLastError(incomingError);
    // Cairo retains the surface. Only the extra reference to our clone is ours.
    void* result = original(copy ? copy : source);
    const auto outgoingError = GetLastError();
    if (copy) thumb_png::detail::release(api, copy);
    SetLastError(outgoingError);
    return result;
}
} // namespace enterprise_navigation
