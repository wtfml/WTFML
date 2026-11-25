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
};
inline const QList<Option> &all() {
    static const QList<Option> options = [] {
        QList<Option> result;
        auto add = [&](const char *key, const QString &name, const QString &group, const char *initial,
                       Kind kind, double low = 0, double high = 1, QList<Choice> choices = {}) {
            result.append({QString::fromLatin1(key), name, group,
                           QString::fromLatin1(initial), kind, low, high, choices});
        };
        add("fov", QCoreApplication::translate("PresetOptionCatalog", "视场角"), QCoreApplication::translate("PresetOptionCatalog", "视频设置"), "0.0", Kind::Integer, 30, 110);
        add("guiScale", QCoreApplication::translate("PresetOptionCatalog", "界面尺寸"), QCoreApplication::translate("PresetOptionCatalog", "视频设置"), "0", Kind::Integer, 0, 16);
        add("renderDistance", QCoreApplication::translate("PresetOptionCatalog", "渲染距离"), QCoreApplication::translate("PresetOptionCatalog", "视频设置"), "12", Kind::Integer, 2, 64);
        add("simulationDistance", QCoreApplication::translate("PresetOptionCatalog", "模拟距离"), QCoreApplication::translate("PresetOptionCatalog", "视频设置"), "12", Kind::Integer, 2, 32);
        add("maxFps", QCoreApplication::translate("PresetOptionCatalog", "最大帧率"), QCoreApplication::translate("PresetOptionCatalog", "视频设置"), "120", Kind::Integer, 10, 260);
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
        add("invertYMouse", QCoreApplication::translate("PresetOptionCatalog", "鼠标反转"), QCoreApplication::translate("PresetOptionCatalog", "鼠标设置"), "false", Kind::Boolean);
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
            QCoreApplication::translate("PresetOptionCatalog", "按键控制"), "right", Kind::Choice, 0, 1,
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

        const QString keysGroup = QCoreApplication::translate("PresetOptionCatalog", "按键绑定");
        add("key_key.forward", QCoreApplication::translate("PresetOptionCatalog", "向前移动"), keysGroup, "key.keyboard.w", Kind::Key);
        add("key_key.back", QCoreApplication::translate("PresetOptionCatalog", "向后移动"), keysGroup, "key.keyboard.s", Kind::Key);
        add("key_key.left", QCoreApplication::translate("PresetOptionCatalog", "向左移动"), keysGroup, "key.keyboard.a", Kind::Key);
        add("key_key.right", QCoreApplication::translate("PresetOptionCatalog", "向右移动"), keysGroup, "key.keyboard.d", Kind::Key);
        add("key_key.jump", QCoreApplication::translate("PresetOptionCatalog", "跳跃"), keysGroup, "key.keyboard.space", Kind::Key);
        add("key_key.sneak", QCoreApplication::translate("PresetOptionCatalog", "潜行"), keysGroup, "key.keyboard.left.shift", Kind::Key);
        add("key_key.sprint", QCoreApplication::translate("PresetOptionCatalog", "疾跑"), keysGroup, "key.keyboard.left.control", Kind::Key);
        add("key_key.inventory", QCoreApplication::translate("PresetOptionCatalog", "开启/关闭物品栏"), keysGroup, "key.keyboard.e", Kind::Key);
        add("key_key.drop", QCoreApplication::translate("PresetOptionCatalog", "丢弃所选物品"), keysGroup, "key.keyboard.q", Kind::Key);
        add("key_key.swapOffhand", QCoreApplication::translate("PresetOptionCatalog", "与副手交换物品"), keysGroup, "key.keyboard.f", Kind::Key);
        add("key_key.chat", QCoreApplication::translate("PresetOptionCatalog", "打开聊天栏"), keysGroup, "key.keyboard.t", Kind::Key);
        add("key_key.command", QCoreApplication::translate("PresetOptionCatalog", "输入命令"), keysGroup, "key.keyboard.slash", Kind::Key);
        add("key_key.playerlist", QCoreApplication::translate("PresetOptionCatalog", "显示玩家列表"), keysGroup, "key.keyboard.tab", Kind::Key);
        add("key_key.togglePerspective", QCoreApplication::translate("PresetOptionCatalog", "切换视角"), keysGroup, "key.keyboard.f5", Kind::Key);
        add("key_key.screenshot", QCoreApplication::translate("PresetOptionCatalog", "截图"), keysGroup, "key.keyboard.f2", Kind::Key);
        add("key_key.fullscreen", QCoreApplication::translate("PresetOptionCatalog", "切换全屏显示"), keysGroup, "key.keyboard.f11", Kind::Key);
        add("key_key.advancements", QCoreApplication::translate("PresetOptionCatalog", "进度"), keysGroup, "key.keyboard.l", Kind::Key);
        result.append({"key_key.attack", QCoreApplication::translate("PresetOptionCatalog", "攻击/摧毁"), QCoreApplication::translate("PresetOptionCatalog", "按键绑定"), "key.mouse.left", Kind::Key});
        result.append({"key_key.use", QCoreApplication::translate("PresetOptionCatalog", "使用物品/放置方块"), QCoreApplication::translate("PresetOptionCatalog", "按键绑定"), "key.mouse.right", Kind::Key});
        result.append({"key_key.pickItem", QCoreApplication::translate("PresetOptionCatalog", "选取方块"), QCoreApplication::translate("PresetOptionCatalog", "按键绑定"), "key.mouse.middle", Kind::Key});
        for (int i = 1; i <= 9; ++i)
            result.append({"key_key.hotbar." + QString::number(i), QCoreApplication::translate("PresetOptionCatalog", "快捷栏%1").arg(i),
                           QCoreApplication::translate("PresetOptionCatalog", "按键绑定"), "key.keyboard." + QString::number(i), Kind::Key});
        return result;
    }();
    return options;
}
inline const Option *find(const QString &key) {
    for (const auto &option : all()) if (option.key == key) return &option;
    return nullptr;
}
}

