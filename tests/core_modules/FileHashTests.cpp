#include "Support.hpp"

void runFileHashTests() {
    section("ZAC. 文件哈希：三档摘要 / 已知值归一化 / 结论 / 真读一个文件（工具箱第 1 件）");

    // ---------- ZAC1. 三档的名字与位数 ----------
    {
        const QList<FileHash::Algorithm> algorithms = FileHash::algorithms();
        check(algorithms == QList<FileHash::Algorithm>({FileHash::Algorithm::Sha256,
                                                       FileHash::Algorithm::Sha1,
                                                       FileHash::Algorithm::Md5}),
              "ZAC1 三档的次序就是界面上那一排（SHA-256 在最前面，是大家默认要的那一个）",
              QStringLiteral("%1 档").arg(algorithms.size()));
        check(FileHash::algorithmName(FileHash::Algorithm::Sha256) == QStringLiteral("SHA-256")
                  && FileHash::algorithmName(FileHash::Algorithm::Sha1) == QStringLiteral("SHA-1")
                  && FileHash::algorithmName(FileHash::Algorithm::Md5) == QStringLiteral("MD5"),
              "ZAC1 界面上的名字：SHA-256 / SHA-1 / MD5");
        check(FileHash::algorithmKey(FileHash::Algorithm::Sha256) == QStringLiteral("sha256")
                  && FileHash::algorithmKey(FileHash::Algorithm::Md5) == QStringLiteral("md5"),
              "ZAC1 配置那侧的名字是小写");
        check(FileHash::hexLength(FileHash::Algorithm::Sha256) == 64
                  && FileHash::hexLength(FileHash::Algorithm::Sha1) == 40
                  && FileHash::hexLength(FileHash::Algorithm::Md5) == 32,
              "ZAC1 三档的位数：64 / 40 / 32");
    }

    // ---------- ZAC2. 真摘要（NIST / RFC 那几条公开向量）----------
    {
        check(FileHash::digestOf(QByteArray(), FileHash::Algorithm::Sha256)
                  == QStringLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
              "ZAC2 SHA-256("") 就是那串 e3b0c442…（空输入）",
              FileHash::digestOf(QByteArray(), FileHash::Algorithm::Sha256));
        check(FileHash::digestOf(QByteArray("abc"), FileHash::Algorithm::Sha256)
                  == QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
              "ZAC2 SHA-256(\"abc\") = ba7816bf…",
              FileHash::digestOf(QByteArray("abc"), FileHash::Algorithm::Sha256));
        check(FileHash::digestOf(QByteArray("abc"), FileHash::Algorithm::Sha1)
                  == QStringLiteral("a9993e364706816aba3e25717850c26c9cd0d89d"),
              "ZAC2 SHA-1(\"abc\") = a9993e36…",
              FileHash::digestOf(QByteArray("abc"), FileHash::Algorithm::Sha1));
        check(FileHash::digestOf(QByteArray("abc"), FileHash::Algorithm::Md5)
                  == QStringLiteral("900150983cd24fb0d6963f7d28e17f72"),
              "ZAC2 MD5(\"abc\") = 90015098…",
              FileHash::digestOf(QByteArray("abc"), FileHash::Algorithm::Md5));
        check(FileHash::digestOf(QByteArray("abc"), FileHash::Algorithm::Sha256).size() == 64
                  && FileHash::digestOf(QByteArray("abc"), FileHash::Algorithm::Sha256)
                         == FileHash::digestOf(QByteArray("abc"), FileHash::Algorithm::Sha256)
                            .toLower(),
              "ZAC2 出去的是**小写**十六进制（后面比那一半靠这个）");
    }

    // ---------- ZAC3. 用户粘进来的"已知值" ----------
    {
        const QString sha256 = QStringLiteral(
            "3f79bb7b435b05321651daefd374cdc681dc06faa65e374e38337b88ca046dea");

        const FileHash::Expected plain = FileHash::parseExpected(sha256);
        check(plain.valid && plain.hex == sha256 && !plain.hasAlgorithm
                  && plain.algorithm == FileHash::Algorithm::Sha256,
              "ZAC3 裸的小写 64 位 ⇒ 合法，长度自己说了是哪一档");

        const FileHash::Expected upper = FileHash::parseExpected(sha256.toUpper());
        check(upper.valid && upper.hex == sha256,
              "ZAC3 全大写 ⇒ 归一化成小写（大小写不是「不同的摘要」）", upper.hex);

        QStringList dashed;
        for (int index = 0; index < sha256.size(); index += 2) {
            dashed << sha256.mid(index, 2);
        }
        const FileHash::Expected hyphens = FileHash::parseExpected(dashed.join(QChar('-')).toUpper());
        check(hyphens.valid && hyphens.hex == sha256,
              "★ ZAC3 下载页那种 `A1-B2-…` 带连字符的大写写法也算同一串", hyphens.error);

        const FileHash::Expected spaced =
            FileHash::parseExpected(QStringLiteral("  sha256: ") + sha256.left(32) + QChar('\n')
                                    + sha256.mid(32) + QStringLiteral("  "));
        check(spaced.valid && spaced.hex == sha256 && spaced.hasAlgorithm
                  && spaced.algorithm == FileHash::Algorithm::Sha256,
              "★ ZAC3 带 `sha256:` 前缀、中间还夹着换行和空格的写法照样认（从网页上复制就是这个样子）",
              spaced.error);

        const FileHash::Expected named = FileHash::parseExpected(QStringLiteral("SHA-256 = ") + sha256);
        check(named.valid && named.hasAlgorithm && named.algorithm == FileHash::Algorithm::Sha256,
              "ZAC3 `SHA-256 = <那一串>` 这种写法也认", named.error);

        const FileHash::Expected md5 = FileHash::parseExpected(
            QStringLiteral("md5:900150983cd24fb0d6963f7d28e17f72"));
        check(md5.valid && md5.algorithm == FileHash::Algorithm::Md5,
              "ZAC3 点了名的那一档按点名的算（`md5:` 那一条）", md5.error);

        const FileHash::Expected sha1 =
            FileHash::parseExpected(QStringLiteral("a9993e364706816aba3e25717850c26c9cd0d89d"));
        check(sha1.valid && sha1.algorithm == FileHash::Algorithm::Sha1,
              "ZAC3 40 位 ⇒ 自己认出来是 SHA-1（用户没写算法也能对上）");

        const FileHash::Expected notHex =
            FileHash::parseExpected(QStringLiteral("zzz9bb7b435b05321651daefd374cdc681dc06faa65e374e38337b88ca046dea"));
        check(!notHex.valid && notHex.error.contains(QStringLiteral("十六进制")),
              "★ ZAC3 不是十六进制的字符 ⇒ 报出来（不是「不一致」，是这一格本身读不懂）", notHex.error);

        const FileHash::Expected wrongLength = FileHash::parseExpected(sha256.left(63));
        check(!wrongLength.valid && wrongLength.error.contains(QStringLiteral("63 个字符")),
              "★ ZAC3 长度对不上任何一档 ⇒ 报长度（63 位既不是 64 也不是 40/32）", wrongLength.error);

        const FileHash::Expected mismatch =
            FileHash::parseExpected(QStringLiteral("sha256:") + sha1.hex);
        check(!mismatch.valid && mismatch.error.contains(QStringLiteral("SHA-256")),
              "★ ZAC3 点名的算法和长度对不上 ⇒ 明说是哪一处对不上", mismatch.error);

        // R12：算法名与摘要之间**只隔一个空格**（文档里写明的写法）。以前先 trimmed()
        // 再把空格当分隔符判，这一支永远进不去 —— `md5 <hex>` 直接被报成
        // 「`m` 不是十六进制的字符」。
        const FileHash::Expected spaceSeparated = FileHash::parseExpected(
            QStringLiteral("md5 900150983cd24fb0d6963f7d28e17f72"));
        check(spaceSeparated.valid && spaceSeparated.hasAlgorithm
                  && spaceSeparated.algorithm == FileHash::Algorithm::Md5
                  && spaceSeparated.hex == QStringLiteral("900150983cd24fb0d6963f7d28e17f72"),
              "★★ R12 `md5 <hex>` 这种只隔一个空格的写法认得出（不再报 `m` 不是十六进制）",
              spaceSeparated.error);
        // 制表符也是空白分隔（同一支）。`sha256something` 仍然不是一个前缀。
        const FileHash::Expected tabSeparated = FileHash::parseExpected(
            QStringLiteral("SHA-256\t") + sha256);
        check(tabSeparated.valid && tabSeparated.hasAlgorithm,
              "★ R12 制表符分隔也认", tabSeparated.error);
        const FileHash::Expected glued = FileHash::parseExpected(
            QStringLiteral("sha256") + sha256);
        check(!glued.valid && glued.error.contains(QStringLiteral("十六进制")),
              "★ R12 算法名和摘要**粘在一起**时不算点名（否则 `sha256abc…` 会被当成算法前缀）",
              glued.error);

        const FileHash::Expected blank = FileHash::parseExpected(QStringLiteral("   "));
        check(!blank.valid && blank.error.isEmpty(),
              "★ ZAC3 空着**不算错**（「我只想看摘要」是正常用法），所以不给错误那句话");
    }

    // ---------- ZAC4. 比一下 ----------
    {
        const FileHash::Expected expected =
            FileHash::parseExpected(QStringLiteral("sha256:")
                                    + FileHash::digestOf(QByteArray("abc"), FileHash::Algorithm::Sha256));
        const FileHash::Verdict match =
            FileHash::compare(FileHash::digestOf(QByteArray("abc"), FileHash::Algorithm::Sha256), expected);
        check(match.valid && match.matches && match.text == QStringLiteral("一致。"),
              "★ ZAC4 一致就说「一致。」", match.text);
        const FileHash::Verdict upperCaseActual = FileHash::compare(
            FileHash::digestOf(QByteArray("abc"), FileHash::Algorithm::Sha256).toUpper(), expected);
        check(upperCaseActual.matches,
              "★ ZAC4 我们算出来的那串大写也照样算一致（比的是十六进制本身，不是字符串）");
        const FileHash::Verdict differ =
            FileHash::compare(FileHash::digestOf(QByteArray("abd"), FileHash::Algorithm::Sha256),
                              expected);
        check(differ.valid && !differ.matches && differ.text.contains(QStringLiteral("不一致")),
              "ZAC4 不一致时说「不一致」并把算出来那一串带上（用户要拿去核）", differ.text);
        check(differ.expectedAlgorithm == FileHash::Algorithm::Sha256 && differ.expectedHasAlgorithm,
              "★ ZAC4 结论里带着「已知值点的是哪一档」（比不同档的摘要没有意义，界面要拿它提示）");
        const FileHash::Verdict noExpected = FileHash::compare(
            FileHash::digestOf(QByteArray("abc"), FileHash::Algorithm::Sha256), FileHash::Expected{});
        check(!noExpected.valid && noExpected.text == QStringLiteral("无已知值可比。"),
              "ZAC4 没给已知值 ⇒ 只说「没得比」，不当成「不一致」", noExpected.text);
    }

    // ---------- ZAC5. 真读一个文件（进度 / 取消 / 读不动）----------
    {
        QTemporaryDir temp;
        check(temp.isValid(), "ZAC5 临时目录建出来了", temp.path());
        if (temp.isValid()) {
            const QByteArray content("hello world\n");
            const QString path = temp.path() + QStringLiteral("/wtfml-toolbox-hash.txt");
            writeFileAt(path, content);

            qint64 lastDone = -1;
            qint64 lastTotal = -1;
            int progressCalls = 0;
            const FileHash::Result result = FileHash::digestOfFile(
                path, FileHash::Algorithm::Sha256,
                [&](qint64 done, qint64 total) {
                    ++progressCalls;
                    lastDone = done;
                    lastTotal = total;
                });
            check(result.ok && result.bytes == content.size()
                      && result.hex == FileHash::digestOf(content, FileHash::Algorithm::Sha256),
                  "★ ZAC5 文件那一档和「给一段字节算」给出同一个摘要（流式分块没有算错）",
                  result.ok ? result.hex : result.error);
            check(progressCalls >= 1 && lastDone == content.size() && lastTotal == content.size(),
                  "ZAC5 进度回调报的是「读了多少 / 一共多少」",
                  QStringLiteral("%1 次，最后一次 %2/%3").arg(progressCalls).arg(lastDone).arg(lastTotal));

            const FileHash::Result missing = FileHash::digestOfFile(
                temp.path() + QStringLiteral("/wtfml-toolbox-nope.bin"), FileHash::Algorithm::Md5);
            check(!missing.ok && !missing.error.isEmpty() && missing.hex.isEmpty()
                      && missing.bytes == -1,
                  "★ ZAC5 打不开 ⇒ ok=false + 一句人话 + hex 空 + 大小 -1", missing.error);

            // 取消：造一个 3 MiB 的文件，读到第二块时喊停。
            QByteArray big;
            big.resize(3 * FileHash::kChunkBytes);
            for (int index = 0; index < big.size(); ++index) {
                big[index] = char(index % 251);
            }
            const QString bigPath = temp.path() + QStringLiteral("/wtfml-toolbox-big.bin");
            writeFileAt(bigPath, big);
            int seen = 0;
            const FileHash::Result cancelled = FileHash::digestOfFile(
                bigPath, FileHash::Algorithm::Sha256, [&seen](qint64, qint64) { ++seen; },
                [&seen]() { return seen >= 1; });
            check(!cancelled.ok && cancelled.error == QStringLiteral("已取消") && cancelled.hex.isEmpty(),
                  "★ ZAC5 用户点了取消 ⇒ 停下来、写「已取消」、不给半截摘要",
                  QStringLiteral("error=%1 seen=%2").arg(cancelled.error).arg(seen));
            check(seen < 3, "★ ZAC5 取消是**真的**停下了（3 MiB 三块，只读了第一块）",
                  QStringLiteral("读了 %1 块").arg(seen));
        }
    }

    // ---------- ZAC6. 文件多大写成什么 ----------
    {
        check(FileHash::fileSizeText(0) == QStringLiteral("0 B")
                  && FileHash::fileSizeText(1023) == QStringLiteral("1023 B"),
              "ZAC6 不到 1 KB 就写字节");
        check(FileHash::fileSizeText(1024) == QStringLiteral("1.0 KB")
                  && FileHash::fileSizeText(1536) == QStringLiteral("1.5 KB"),
              "ZAC6 KB 那一档一位小数");
        check(FileHash::fileSizeText(1024 * 1024) == QStringLiteral("1.0 MB")
                  && FileHash::fileSizeText(3 * 1024 * 1024) == QStringLiteral("3.0 MB"),
              "ZAC6 MB 那一档（整包几百 MB 就是它）");
        check(FileHash::fileSizeText(-1).isEmpty(),
              "★ ZAC6 问不出来（-1）回空串，不写「-1 B」", FileHash::fileSizeText(-1));
    }
}

