#pragma once
#include <QString>
#include <QCoreApplication>
#include <QList>

namespace PresetOptionCatalog {
enum class Kind { Boolean, Integer, Fraction, Choice, Key };
struct Choice {
    const char *value;
    QString label;
};
struct Option {
    QString key, name, group, initial;
    Kind kind;
    double minimum = 0, maximum = 1;
    QList<Choice> choices = {};
    double step = 0;
};
inline const QList<Option> &all() {
    static const QList<Option> options = [] {
        QList<Option> result;
        auto add = [&](const char *key, const QString &name, const QString &group, const char *initial,
                       Kind kind, double low = 0, double high = 1, QList<Choice> choices = {}, double step = 0) {
            result.append({QString::fromLatin1(key), name, group,
                           QString::fromLatin1(initial), kind, low, high, choices, step});
        };
        add("fov", QCoreApplication::translate("PresetOptionCatalog", "视场角"), QCoreApplication::translate("PresetOptionCatalog", "视频设置"), "0.0", Kind::Integer, 30, 110);
        add("guiScale", QCoreApplication::translate("PresetOptionCatalog", "界面尺寸"), QCoreApplication::translate("PresetOptionCatalog", "视频设置"), "0", Kind::Integer, 0, 16);
        add("renderDistance", QCoreApplication::translate("PresetOptionCatalog", "渲染距离"), QCoreApplication::translate("PresetOptionCatalog", "视频设置"), "12", Kind::Integer, 2, 64);
        add("simulationDistance", QCoreApplication::translate("PresetOptionCatalog", "模拟距离"), QCoreApplication::translate("PresetOptionCatalog", "视频设置"), "12", Kind::Integer, 2, 32);
        add("maxFps", QCoreApplication::translate("PresetOptionCatalog", "最大帧率"), QCoreApplication::translate("PresetOptionCatalog", "视频设置"), "120", Kind::Integer, 10, 260, {}, 10);
        add("gamma", QCoreApplication::translate("PresetOptionCatalog", "亮度"), QCoreApplication::translate("PresetOptionCatalog", "视频设置"), "0.5", Kind::Fraction);
        add("enableVsync", QCoreApplication::translate("PresetOptionCatalog", "垂直同步"), QCoreApplication::translate("PresetOptionCatalog", "视频设置"), "true", Kind::Boolean);
        add("fullscreen", QCoreApplication::translate("PresetOptionCatalog", "全屏"), QCoreApplication::translate("PresetOptionCatalog", "视频设置"), "false", Kind::Boolean);
        add("graphicsMode", QCoreApplication::translate("PresetOptionCatalog", "图像品质"),
            QCoreApplication::translate("PresetOptionCatalog", "视频设置"), "1", Kind::Choice, 0, 1,
            {{"0", QCoreApplication::translate("PresetOptionCatalog", "流畅")},
             {"1", QCoreApplication::translate("PresetOptionCatalog", "高品质")},
             {"2", QCoreApplication::translate("PresetOptionCatalog", "极佳！")}});
        add("particles", QCoreApplication::translate("PresetOptionCatalog", "粒子效果"),
            QCoreApplication::translate("PresetOptionCatalog", "视频设置"), "0", Kind::Choice, 0, 1,
            {{"0", QCoreApplication::translate("PresetOptionCatalog", "全部")},
             {"1", QCoreApplication::translate("PresetOptionCatalog", "少量")},
             {"2", QCoreApplication::translate("PresetOptionCatalog", "最少")}});
        add("renderClouds", QCoreApplication::translate("PresetOptionCatalog", "云"),
            QCoreApplication::translate("PresetOptionCatalog", "视频设置"), "true", Kind::Choice, 0, 1,
            {{"false", QCoreApplication::translate("PresetOptionCatalog", "关")},
             {"fast", QCoreApplication::translate("PresetOptionCatalog", "流畅")},
             {"true", QCoreApplication::translate("PresetOptionCatalog", "高品质")}});
        add("entityShadows", QCoreApplication::translate("PresetOptionCatalog", "实体阴影"), QCoreApplication::translate("PresetOptionCatalog", "视频设置"), "true", Kind::Boolean);
        add("bobView", QCoreApplication::translate("PresetOptionCatalog", "视角摇晃"), QCoreApplication::translate("PresetOptionCatalog", "视频设置"), "true", Kind::Boolean);
        add("autoJump", QCoreApplication::translate("PresetOptionCatalog", "自动跳跃"), QCoreApplication::translate("PresetOptionCatalog", "按键控制"), "false", Kind::Boolean);
        add("mouseSensitivity", QCoreApplication::translate("PresetOptionCatalog", "鼠标灵敏度"), QCoreApplication::translate("PresetOptionCatalog", "鼠标设置"), "0.5", Kind::Fraction);
        add("invertYMouse", QCoreApplication::translate("PresetOptionCatalog", "反转鼠标Y轴"), QCoreApplication::translate("PresetOptionCatalog", "鼠标设置"), "false", Kind::Boolean);
        add("rawMouseInput", QCoreApplication::translate("PresetOptionCatalog", "原始输入"), QCoreApplication::translate("PresetOptionCatalog", "鼠标设置"), "true", Kind::Boolean);
        const QList<Choice> holdOrToggle{
            {"false", QCoreApplication::translate("PresetOptionCatalog", "按住")},
            {"true", QCoreApplication::translate("PresetOptionCatalog", "切换")},
        };
        add("toggleCrouch", QCoreApplication::translate("PresetOptionCatalog", "潜行"),
            QCoreApplication::translate("PresetOptionCatalog", "按键控制"), "false", Kind::Choice, 0, 1, holdOrToggle);
        add("toggleSprint", QCoreApplication::translate("PresetOptionCatalog", "疾跑"),
            QCoreApplication::translate("PresetOptionCatalog", "按键控制"), "false", Kind::Choice, 0, 1, holdOrToggle);
        add("mainHand", QCoreApplication::translate("PresetOptionCatalog", "主手"),
            QCoreApplication::translate("PresetOptionCatalog", "自定义皮肤"), "right", Kind::Choice, 0, 1,
            {{"right", QCoreApplication::translate("PresetOptionCatalog", "右手")},
             {"left", QCoreApplication::translate("PresetOptionCatalog", "左手")}});
        add("pauseOnLostFocus", QCoreApplication::translate("PresetOptionCatalog", "失去焦点时暂停"), QCoreApplication::translate("PresetOptionCatalog", "按键控制"), "true", Kind::Boolean);
        add("showSubtitles", QCoreApplication::translate("PresetOptionCatalog", "隐藏式字幕"), QCoreApplication::translate("PresetOptionCatalog", "辅助功能设置"), "false", Kind::Boolean);
        add("highContrast", QCoreApplication::translate("PresetOptionCatalog", "高对比度"), QCoreApplication::translate("PresetOptionCatalog", "辅助功能设置"), "false", Kind::Boolean);
        add("hideLightningFlash", QCoreApplication::translate("PresetOptionCatalog", "隐藏天空闪烁效果"), QCoreApplication::translate("PresetOptionCatalog", "辅助功能设置"), "false", Kind::Boolean);
        add("screenEffectScale", QCoreApplication::translate("PresetOptionCatalog", "屏幕扭曲效果"), QCoreApplication::translate("PresetOptionCatalog", "辅助功能设置"), "1.0", Kind::Fraction);
        add("fovEffectScale", QCoreApplication::translate("PresetOptionCatalog", "视场角效果"), QCoreApplication::translate("PresetOptionCatalog", "辅助功能设置"), "1.0", Kind::Fraction);
        add("darknessEffectScale", QCoreApplication::translate("PresetOptionCatalog", "黑暗脉动效果"), QCoreApplication::translate("PresetOptionCatalog", "辅助功能设置"), "1.0", Kind::Fraction);
        add("chatVisibility", QCoreApplication::translate("PresetOptionCatalog", "聊天"),
            QCoreApplication::translate("PresetOptionCatalog", "聊天设置"), "0", Kind::Choice, 0, 1,
            {{"0", QCoreApplication::translate("PresetOptionCatalog", "显示")},
             {"1", QCoreApplication::translate("PresetOptionCatalog", "仅限命令")},
             {"2", QCoreApplication::translate("PresetOptionCatalog", "隐藏")}});
        add("chatColors", QCoreApplication::translate("PresetOptionCatalog", "颜色"), QCoreApplication::translate("PresetOptionCatalog", "聊天设置"), "true", Kind::Boolean);
        add("chatLinks", QCoreApplication::translate("PresetOptionCatalog", "网页链接"), QCoreApplication::translate("PresetOptionCatalog", "聊天设置"), "true", Kind::Boolean);
        add("chatLinksPrompt", QCoreApplication::translate("PresetOptionCatalog", "链接提示"), QCoreApplication::translate("PresetOptionCatalog", "聊天设置"), "true", Kind::Boolean);
        add("chatOpacity", QCoreApplication::translate("PresetOptionCatalog", "聊天文本不透明度"), QCoreApplication::translate("PresetOptionCatalog", "聊天设置"), "1.0", Kind::Fraction);
        add("chatScale", QCoreApplication::translate("PresetOptionCatalog", "聊天文本大小"), QCoreApplication::translate("PresetOptionCatalog", "聊天设置"), "1.0", Kind::Fraction);
        const QString soundGroup = QCoreApplication::translate("PresetOptionCatalog", "音乐和声音选项");
        add("soundCategory_master", QCoreApplication::translate("PresetOptionCatalog", "主音量"), soundGroup, "1.0", Kind::Fraction);
        add("soundCategory_music", QCoreApplication::translate("PresetOptionCatalog", "音乐"), soundGroup, "1.0", Kind::Fraction);
        add("soundCategory_record", QCoreApplication::translate("PresetOptionCatalog", "唱片机/音符盒"), soundGroup, "1.0", Kind::Fraction);
        add("soundCategory_weather", QCoreApplication::translate("PresetOptionCatalog", "天气"), soundGroup, "1.0", Kind::Fraction);
        add("soundCategory_block", QCoreApplication::translate("PresetOptionCatalog", "方块"), soundGroup, "1.0", Kind::Fraction);
        add("soundCategory_hostile", QCoreApplication::translate("PresetOptionCatalog", "敌对生物"), soundGroup, "1.0", Kind::Fraction);
        add("soundCategory_neutral", QCoreApplication::translate("PresetOptionCatalog", "友好生物"), soundGroup, "1.0", Kind::Fraction);
        add("soundCategory_player", QCoreApplication::translate("PresetOptionCatalog", "玩家"), soundGroup, "1.0", Kind::Fraction);
        add("soundCategory_ambient", QCoreApplication::translate("PresetOptionCatalog", "环境"), soundGroup, "1.0", Kind::Fraction);
        add("soundCategory_voice", QCoreApplication::translate("PresetOptionCatalog", "复述功能/语音"), soundGroup, "1.0", Kind::Fraction);

        // 26.2 各设置菜单中的可携带选项，初值和档位取自原版。
        const QString controlsGroup = QCoreApplication::translate("PresetOptionCatalog", "按键控制");
        add("toggleAttack", QCoreApplication::translate("PresetOptionCatalog", "攻击/摧毁"), controlsGroup, "false", Kind::Choice, 0, 1, {
            {"false", QCoreApplication::translate("PresetOptionCatalog", "按住")},
            {"true", QCoreApplication::translate("PresetOptionCatalog", "切换")}
        });
        add("toggleUse", QCoreApplication::translate("PresetOptionCatalog", "使用物品/放置方块"), controlsGroup, "false", Kind::Choice, 0, 1, {
            {"false", QCoreApplication::translate("PresetOptionCatalog", "按住")},
            {"true", QCoreApplication::translate("PresetOptionCatalog", "切换")}
        });
        add("sprintWindow", QCoreApplication::translate("PresetOptionCatalog", "疾跑间隔"), controlsGroup, "7", Kind::Integer, 0, 10);
        add("operatorItemsTab", QCoreApplication::translate("PresetOptionCatalog", "管理员用品标签页"), controlsGroup, "false", Kind::Boolean);

        const QString mouseGroup = QCoreApplication::translate("PresetOptionCatalog", "鼠标设置");
        add("mouseWheelSensitivity", QCoreApplication::translate("PresetOptionCatalog", "滚轮灵敏度"), mouseGroup, "1.0", Kind::Fraction, 0.01, 10);
        add("discrete_mouse_scroll", QCoreApplication::translate("PresetOptionCatalog", "离散滚动"), mouseGroup, "false", Kind::Boolean);
        add("invertXMouse", QCoreApplication::translate("PresetOptionCatalog", "反转鼠标X轴"), mouseGroup, "false", Kind::Boolean);
        add("allowCursorChanges", QCoreApplication::translate("PresetOptionCatalog", "允许光标变化"), mouseGroup, "true", Kind::Boolean);

        const QString videoGroup = QCoreApplication::translate("PresetOptionCatalog", "视频设置");
        add("biomeBlendRadius", QCoreApplication::translate("PresetOptionCatalog", "生物群系过渡距离"), videoGroup, "2", Kind::Integer, 0, 7);
        add("prioritizeChunkUpdates", QCoreApplication::translate("PresetOptionCatalog", "区块构建器"), videoGroup, "0", Kind::Choice, 0, 1, {
            {"0", QCoreApplication::translate("PresetOptionCatalog", "线程化")},
            {"1", QCoreApplication::translate("PresetOptionCatalog", "半阻塞")},
            {"2", QCoreApplication::translate("PresetOptionCatalog", "全阻塞")}
        });
        add("ao", QCoreApplication::translate("PresetOptionCatalog", "平滑光照"), videoGroup, "true", Kind::Boolean);
        add("mipmapLevels", QCoreApplication::translate("PresetOptionCatalog", "Mipmap级别"), videoGroup, "4", Kind::Integer, 0, 4);
        add("entityDistanceScaling", QCoreApplication::translate("PresetOptionCatalog", "实体渲染距离"), videoGroup, "1.0", Kind::Fraction, 0.5, 5, {}, 0.25);
        add("cloudRange", QCoreApplication::translate("PresetOptionCatalog", "云层距离"), videoGroup, "128", Kind::Integer, 2, 128);
        add("cutoutLeaves", QCoreApplication::translate("PresetOptionCatalog", "可透视树叶"), videoGroup, "true", Kind::Boolean);
        add("improvedTransparency", QCoreApplication::translate("PresetOptionCatalog", "改进透明显示"), videoGroup, "false", Kind::Boolean);
        add("textureFiltering", QCoreApplication::translate("PresetOptionCatalog", "纹理过滤"), videoGroup, "0", Kind::Choice, 0, 1, {
            {"0", QCoreApplication::translate("PresetOptionCatalog", "无")},
            {"1", QCoreApplication::translate("PresetOptionCatalog", "RGSS")},
            {"2", QCoreApplication::translate("PresetOptionCatalog", "各向异性")}
        });
        add("maxAnisotropyBit", QCoreApplication::translate("PresetOptionCatalog", "各向异性过滤"), videoGroup, "2", Kind::Integer, 1, 3);
        add("weatherRadius", QCoreApplication::translate("PresetOptionCatalog", "天气效果半径"), videoGroup, "10", Kind::Integer, 3, 10);
        add("inactivityFpsLimit", QCoreApplication::translate("PresetOptionCatalog", "降低帧率"), videoGroup, "\"afk\"", Kind::Choice, 0, 1, {
            {"\"minimized\"", QCoreApplication::translate("PresetOptionCatalog", "最小化时")},
            {"\"afk\"", QCoreApplication::translate("PresetOptionCatalog", "无操作时")}
        });
        add("exclusiveFullscreen", QCoreApplication::translate("PresetOptionCatalog", "独占全屏"), videoGroup, "false", Kind::Boolean);
        add("showAutosaveIndicator", QCoreApplication::translate("PresetOptionCatalog", "自动保存指示器"), videoGroup, "true", Kind::Boolean);
        add("vignette", QCoreApplication::translate("PresetOptionCatalog", "显示晕影"), videoGroup, "true", Kind::Boolean);
        add("attackIndicator", QCoreApplication::translate("PresetOptionCatalog", "攻击指示器"), videoGroup, "1", Kind::Choice, 0, 1, {
            {"0", QCoreApplication::translate("PresetOptionCatalog", "关")},
            {"1", QCoreApplication::translate("PresetOptionCatalog", "十字准星")},
            {"2", QCoreApplication::translate("PresetOptionCatalog", "快捷栏")}
        });
        add("chunkSectionFadeInTime", QCoreApplication::translate("PresetOptionCatalog", "区块淡入时长"), videoGroup, "0.75", Kind::Fraction, 0, 2, {}, 0.05);
        add("preferredGraphicsBackend", QCoreApplication::translate("PresetOptionCatalog", "图形API"), videoGroup, "\"default\"", Kind::Choice, 0, 1, {
            {"\"default\"", QCoreApplication::translate("PresetOptionCatalog", "默认")},
            {"\"opengl\"", QCoreApplication::translate("PresetOptionCatalog", "优先使用OpenGL")},
            {"\"vulkan\"", QCoreApplication::translate("PresetOptionCatalog", "优先使用Vulkan（实验性）")}
        });

        const QString accessGroup = QCoreApplication::translate("PresetOptionCatalog", "辅助功能设置");
        add("menuBackgroundBlurriness", QCoreApplication::translate("PresetOptionCatalog", "菜单背景模糊程度"), accessGroup, "5", Kind::Integer, 0, 10);
        add("textBackgroundOpacity", QCoreApplication::translate("PresetOptionCatalog", "文本背景不透明度"), accessGroup, "0.5", Kind::Fraction, 0, 1);
        add("backgroundForChatOnly", QCoreApplication::translate("PresetOptionCatalog", "文本背景"), accessGroup, "true", Kind::Choice, 0, 1, {
            {"true", QCoreApplication::translate("PresetOptionCatalog", "聊天")},
            {"false", QCoreApplication::translate("PresetOptionCatalog", "全局")}
        });
        add("narrator", QCoreApplication::translate("PresetOptionCatalog", "复述功能"), accessGroup, "0", Kind::Choice, 0, 1, {
            {"0", QCoreApplication::translate("PresetOptionCatalog", "关")},
            {"1", QCoreApplication::translate("PresetOptionCatalog", "复述所有")},
            {"2", QCoreApplication::translate("PresetOptionCatalog", "复述聊天消息")},
            {"3", QCoreApplication::translate("PresetOptionCatalog", "复述系统消息")}
        });
        add("notificationDisplayTime", QCoreApplication::translate("PresetOptionCatalog", "通知显示时长"), accessGroup, "1.0", Kind::Fraction, 0.5, 10, {}, 0.1);
        add("damageTiltStrength", QCoreApplication::translate("PresetOptionCatalog", "受伤抖动效果"), accessGroup, "1.0", Kind::Fraction, 0, 1);
        add("glintSpeed", QCoreApplication::translate("PresetOptionCatalog", "附魔光效闪烁速度"), accessGroup, "0.5", Kind::Fraction, 0, 1);
        add("glintStrength", QCoreApplication::translate("PresetOptionCatalog", "附魔光效闪烁强度"), accessGroup, "0.75", Kind::Fraction, 0, 1);
        add("darkMojangStudiosBackground", QCoreApplication::translate("PresetOptionCatalog", "黑白徽标"), accessGroup, "false", Kind::Boolean);
        add("panoramaScrollSpeed", QCoreApplication::translate("PresetOptionCatalog", "全景图滚动速度"), accessGroup, "1.0", Kind::Fraction, 0, 1);
        add("hideSplashTexts", QCoreApplication::translate("PresetOptionCatalog", "隐藏闪烁标语"), accessGroup, "false", Kind::Boolean);
        add("narratorHotkey", QCoreApplication::translate("PresetOptionCatalog", "复述功能快捷键"), accessGroup, "true", Kind::Boolean);
        add("rotateWithMinecart", QCoreApplication::translate("PresetOptionCatalog", "同步矿车转动"), accessGroup, "false", Kind::Boolean);
        add("highContrastBlockOutline", QCoreApplication::translate("PresetOptionCatalog", "高对比度方块轮廓"), accessGroup, "false", Kind::Boolean);

        const QString chatGroup = QCoreApplication::translate("PresetOptionCatalog", "聊天设置");
        add("chatLineSpacing", QCoreApplication::translate("PresetOptionCatalog", "行距"), chatGroup, "0.0", Kind::Fraction, 0, 1);
        add("chatDelay", QCoreApplication::translate("PresetOptionCatalog", "聊天延迟"), chatGroup, "0.0", Kind::Fraction, 0, 6, {}, 0.1);
        add("chatWidth", QCoreApplication::translate("PresetOptionCatalog", "宽度"), chatGroup, "1.0", Kind::Fraction, 0, 1);
        add("chatHeightFocused", QCoreApplication::translate("PresetOptionCatalog", "聚焦高度"), chatGroup, "1.0", Kind::Fraction, 0, 1);
        add("chatHeightUnfocused", QCoreApplication::translate("PresetOptionCatalog", "淡化高度"), chatGroup, "0.4375", Kind::Fraction, 0, 1);
        add("autoSuggestions", QCoreApplication::translate("PresetOptionCatalog", "命令提示"), chatGroup, "true", Kind::Boolean);
        add("hideMatchedNames", QCoreApplication::translate("PresetOptionCatalog", "隐藏匹配的名称"), chatGroup, "true", Kind::Boolean);
        add("reducedDebugInfo", QCoreApplication::translate("PresetOptionCatalog", "简化调试信息"), chatGroup, "false", Kind::Boolean);
        add("onlyShowSecureChat", QCoreApplication::translate("PresetOptionCatalog", "仅显示安全的聊天"), chatGroup, "false", Kind::Boolean);
        add("saveChatDrafts", QCoreApplication::translate("PresetOptionCatalog", "保存未发送的聊天消息"), chatGroup, "false", Kind::Boolean);

        add("directionalAudio", QCoreApplication::translate("PresetOptionCatalog", "定向音频"), soundGroup, "false", Kind::Boolean);
        add("musicFrequency", QCoreApplication::translate("PresetOptionCatalog", "音乐播放频率"), soundGroup, "\"DEFAULT\"", Kind::Choice, 0, 1, {
            {"\"DEFAULT\"", QCoreApplication::translate("PresetOptionCatalog", "默认")},
            {"\"FREQUENT\"", QCoreApplication::translate("PresetOptionCatalog", "频繁")},
            {"\"CONSTANT\"", QCoreApplication::translate("PresetOptionCatalog", "持续")}
        });
        add("musicToast", QCoreApplication::translate("PresetOptionCatalog", "音乐弹窗"), soundGroup, "\"never\"", Kind::Choice, 0, 1, {
            {"\"never\"", QCoreApplication::translate("PresetOptionCatalog", "不显示")},
            {"\"pause\"", QCoreApplication::translate("PresetOptionCatalog", "暂停菜单")},
            {"\"pause_and_toast\"", QCoreApplication::translate("PresetOptionCatalog", "暂停菜单和弹窗")}
        });

        const QString skinGroup = QCoreApplication::translate("PresetOptionCatalog", "自定义皮肤");
        add("modelPart_cape", QCoreApplication::translate("PresetOptionCatalog", "披风"), skinGroup, "true", Kind::Boolean);
        add("modelPart_jacket", QCoreApplication::translate("PresetOptionCatalog", "外套"), skinGroup, "true", Kind::Boolean);
        add("modelPart_left_sleeve", QCoreApplication::translate("PresetOptionCatalog", "左袖"), skinGroup, "true", Kind::Boolean);
        add("modelPart_right_sleeve", QCoreApplication::translate("PresetOptionCatalog", "右袖"), skinGroup, "true", Kind::Boolean);
        add("modelPart_left_pants_leg", QCoreApplication::translate("PresetOptionCatalog", "左裤腿"), skinGroup, "true", Kind::Boolean);
        add("modelPart_right_pants_leg", QCoreApplication::translate("PresetOptionCatalog", "右裤腿"), skinGroup, "true", Kind::Boolean);
        add("modelPart_hat", QCoreApplication::translate("PresetOptionCatalog", "帽子"), skinGroup, "true", Kind::Boolean);

        // 按游戏中的类别与默认绑定建表，调试快捷键继续保存原版的键标识。
        const QString movementKeys = QCoreApplication::translate("PresetOptionCatalog", "移动");
        add("key_key.forward", QCoreApplication::translate("PresetOptionCatalog", "向前移动"), movementKeys, "key.keyboard.w", Kind::Key);
        add("key_key.left", QCoreApplication::translate("PresetOptionCatalog", "向左移动"), movementKeys, "key.keyboard.a", Kind::Key);
        add("key_key.back", QCoreApplication::translate("PresetOptionCatalog", "向后移动"), movementKeys, "key.keyboard.s", Kind::Key);
        add("key_key.right", QCoreApplication::translate("PresetOptionCatalog", "向右移动"), movementKeys, "key.keyboard.d", Kind::Key);
        add("key_key.jump", QCoreApplication::translate("PresetOptionCatalog", "跳跃"), movementKeys, "key.keyboard.space", Kind::Key);
        add("key_key.sneak", QCoreApplication::translate("PresetOptionCatalog", "潜行"), movementKeys, "key.keyboard.left.shift", Kind::Key);
        add("key_key.sprint", QCoreApplication::translate("PresetOptionCatalog", "疾跑"), movementKeys, "key.keyboard.left.control", Kind::Key);

        const QString multiplayerKeys = QCoreApplication::translate("PresetOptionCatalog", "多人游戏");
        add("key_key.chat", QCoreApplication::translate("PresetOptionCatalog", "打开聊天栏"), multiplayerKeys, "key.keyboard.t", Kind::Key);
        add("key_key.playerlist", QCoreApplication::translate("PresetOptionCatalog", "显示玩家列表"), multiplayerKeys, "key.keyboard.tab", Kind::Key);
        add("key_key.command", QCoreApplication::translate("PresetOptionCatalog", "输入命令"), multiplayerKeys, "key.keyboard.slash", Kind::Key);
        add("key_key.friends", QCoreApplication::translate("PresetOptionCatalog", "好友屏幕"), multiplayerKeys, "key.keyboard.o", Kind::Key);
        add("key_key.socialInteractions", QCoreApplication::translate("PresetOptionCatalog", "社交屏幕"), multiplayerKeys, "key.keyboard.p", Kind::Key);

        const QString gameplayKeys = QCoreApplication::translate("PresetOptionCatalog", "游戏内容");
        add("key_key.use", QCoreApplication::translate("PresetOptionCatalog", "使用物品/放置方块"), gameplayKeys, "key.mouse.right", Kind::Key);
        add("key_key.attack", QCoreApplication::translate("PresetOptionCatalog", "攻击/摧毁"), gameplayKeys, "key.mouse.left", Kind::Key);
        add("key_key.pickItem", QCoreApplication::translate("PresetOptionCatalog", "选取方块"), gameplayKeys, "key.mouse.middle", Kind::Key);

        const QString inventoryKeys = QCoreApplication::translate("PresetOptionCatalog", "物品栏");
        add("key_key.inventory", QCoreApplication::translate("PresetOptionCatalog", "开启/关闭物品栏"), inventoryKeys, "key.keyboard.e", Kind::Key);
        add("key_key.swapOffhand", QCoreApplication::translate("PresetOptionCatalog", "与副手交换物品"), inventoryKeys, "key.keyboard.f", Kind::Key);
        add("key_key.drop", QCoreApplication::translate("PresetOptionCatalog", "丢弃所选物品"), inventoryKeys, "key.keyboard.q", Kind::Key);
        for (int slot = 1; slot <= 9; ++slot)
            result.append({"key_key.hotbar." + QString::number(slot), QCoreApplication::translate("PresetOptionCatalog", "快捷栏%1").arg(slot),
                           inventoryKeys, "key.keyboard." + QString::number(slot), Kind::Key});

        const QString creativeKeys = QCoreApplication::translate("PresetOptionCatalog", "创造模式");
        add("key_key.saveToolbarActivator", QCoreApplication::translate("PresetOptionCatalog", "保存快捷栏"), creativeKeys, "key.keyboard.c", Kind::Key);
        add("key_key.loadToolbarActivator", QCoreApplication::translate("PresetOptionCatalog", "加载快捷栏"), creativeKeys, "key.keyboard.x", Kind::Key);

        const QString spectatorKeys = QCoreApplication::translate("PresetOptionCatalog", "旁观");
        add("key_key.spectatorOutlines", QCoreApplication::translate("PresetOptionCatalog", "高亮玩家"), spectatorKeys, "key.keyboard.unknown", Kind::Key);
        add("key_key.spectatorHotbar", QCoreApplication::translate("PresetOptionCatalog", "呼出快捷栏"), spectatorKeys, "key.mouse.middle", Kind::Key);

        const QString miscKeys = QCoreApplication::translate("PresetOptionCatalog", "杂项");
        add("key_key.screenshot", QCoreApplication::translate("PresetOptionCatalog", "截图"), miscKeys, "key.keyboard.f2", Kind::Key);
        add("key_key.togglePerspective", QCoreApplication::translate("PresetOptionCatalog", "切换视角"), miscKeys, "key.keyboard.f5", Kind::Key);
        add("key_key.smoothCamera", QCoreApplication::translate("PresetOptionCatalog", "切换电影视角"), miscKeys, "key.keyboard.unknown", Kind::Key);
        add("key_key.fullscreen", QCoreApplication::translate("PresetOptionCatalog", "切换全屏显示"), miscKeys, "key.keyboard.f11", Kind::Key);
        add("key_key.advancements", QCoreApplication::translate("PresetOptionCatalog", "进度"), miscKeys, "key.keyboard.l", Kind::Key);
        add("key_key.quickActions", QCoreApplication::translate("PresetOptionCatalog", "快捷操作"), miscKeys, "key.keyboard.g", Kind::Key);
        add("key_key.toggleGui", QCoreApplication::translate("PresetOptionCatalog", "切换界面"), miscKeys, "key.keyboard.f1", Kind::Key);
        add("key_key.toggleSpectatorShaderEffects", QCoreApplication::translate("PresetOptionCatalog", "切换旁观者着色器效果"), miscKeys, "key.keyboard.f4", Kind::Key);

        const QString debugKeys = QCoreApplication::translate("PresetOptionCatalog", "调试");
        add("key_key.debug.overlay", QCoreApplication::translate("PresetOptionCatalog", "切换叠加层"), debugKeys, "key.keyboard.f3", Kind::Key);
        add("key_key.debug.modifier", QCoreApplication::translate("PresetOptionCatalog", "调试修饰键"), debugKeys, "key.keyboard.f3", Kind::Key);
        add("key_key.debug.crash", QCoreApplication::translate("PresetOptionCatalog", "调试崩溃"), debugKeys, "key.keyboard.c", Kind::Key);
        add("key_key.debug.reloadChunk", QCoreApplication::translate("PresetOptionCatalog", "重新加载区块"), debugKeys, "key.keyboard.a", Kind::Key);
        add("key_key.debug.showHitboxes", QCoreApplication::translate("PresetOptionCatalog", "显示判定箱"), debugKeys, "key.keyboard.b", Kind::Key);
        add("key_key.debug.clearChat", QCoreApplication::translate("PresetOptionCatalog", "清除聊天消息"), debugKeys, "key.keyboard.d", Kind::Key);
        add("key_key.debug.showChunkBorders", QCoreApplication::translate("PresetOptionCatalog", "显示区块边界"), debugKeys, "key.keyboard.g", Kind::Key);
        add("key_key.debug.showAdvancedTooltips", QCoreApplication::translate("PresetOptionCatalog", "显示高级提示框"), debugKeys, "key.keyboard.h", Kind::Key);
        add("key_key.debug.copyRecreateCommand", QCoreApplication::translate("PresetOptionCatalog", "复制数据"), debugKeys, "key.keyboard.i", Kind::Key);
        add("key_key.debug.spectate", QCoreApplication::translate("PresetOptionCatalog", "循环切换旁观模式"), debugKeys, "key.keyboard.n", Kind::Key);
        add("key_key.debug.switchGameMode", QCoreApplication::translate("PresetOptionCatalog", "游戏模式切换器"), debugKeys, "key.keyboard.f4", Kind::Key);
        add("key_key.debug.debugOptions", QCoreApplication::translate("PresetOptionCatalog", "调试选项"), debugKeys, "key.keyboard.f6", Kind::Key);
        add("key_key.debug.focusPause", QCoreApplication::translate("PresetOptionCatalog", "切换失去焦点时暂停"), debugKeys, "key.keyboard.p", Kind::Key);
        add("key_key.debug.dumpDynamicTextures", QCoreApplication::translate("PresetOptionCatalog", "转储动态纹理"), debugKeys, "key.keyboard.s", Kind::Key);
        add("key_key.debug.reloadResourcePacks", QCoreApplication::translate("PresetOptionCatalog", "重新加载资源包"), debugKeys, "key.keyboard.t", Kind::Key);
        add("key_key.debug.profiling", QCoreApplication::translate("PresetOptionCatalog", "开始/停止分析"), debugKeys, "key.keyboard.l", Kind::Key);
        add("key_key.debug.copyLocation", QCoreApplication::translate("PresetOptionCatalog", "复制位置"), debugKeys, "key.keyboard.c", Kind::Key);
        add("key_key.debug.dumpVersion", QCoreApplication::translate("PresetOptionCatalog", "转储版本信息"), debugKeys, "key.keyboard.v", Kind::Key);
        add("key_key.debug.profilingChart", QCoreApplication::translate("PresetOptionCatalog", "分析图表"), debugKeys, "key.keyboard.1", Kind::Key);
        add("key_key.debug.fpsCharts", QCoreApplication::translate("PresetOptionCatalog", "FPS图表"), debugKeys, "key.keyboard.2", Kind::Key);
        add("key_key.debug.networkCharts", QCoreApplication::translate("PresetOptionCatalog", "网络图表"), debugKeys, "key.keyboard.3", Kind::Key);
        add("key_key.debug.lightmapTexture", QCoreApplication::translate("PresetOptionCatalog", "光照贴图纹理"), debugKeys, "key.keyboard.4", Kind::Key);

        return result;
    }();
    return options;
}
inline const Option *find(const QString &key) {
    for (const auto &option : all()) if (option.key == key) return &option;
    return nullptr;
}
}

