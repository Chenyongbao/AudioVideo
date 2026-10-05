#include "mainwindow.h"

#include <QApplication>
#include <cstdio>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

int main(int argc, char *argv[])
{
    // FFmpeg 冒烟测试：编译链接+运行时 DLL 全通则打印版本
    fprintf(stderr, "[smoke] FFmpeg runtime: %s | avformat %d.%d.%d\n",
            av_version_info(),
            LIBAVFORMAT_VERSION_MAJOR, LIBAVFORMAT_VERSION_MINOR, LIBAVFORMAT_VERSION_MICRO);

    QApplication a(argc, argv);

    // 深色主题:专业视频软件标配(监控 NVR/证据管理类全是暗色),
    // 视频区域融入背景、控制室长时间盯屏不刺眼。统一在入口处套 QSS。
    a.setStyleSheet(R"(
* { font-family: "Microsoft YaHei UI"; font-size: 12px; color: #d8dee6; }

QMainWindow, QDialog { background: #1b1f24; }

/* ---- 工具栏与按钮 ---- */
QToolBar { background: #232830; border-bottom: 1px solid #343b46; spacing: 6px; padding: 4px; }
QToolButton {
    background: #2c333d; border: 1px solid #3a424e; border-radius: 4px;
    padding: 5px 14px; color: #d8dee6;
}
QToolButton:hover { background: #384250; border-color: #4a5563; }
QToolButton:pressed { background: #26508f; }
QToolButton:checked { background: #2f6fd0; border-color: #3d8bff; color: #ffffff; }
QToolButton:disabled { color: #6b7684; background: #232830; }

QPushButton {
    background: #2c333d; border: 1px solid #3a424e; border-radius: 4px;
    padding: 6px 16px; color: #d8dee6;
}
QPushButton:hover { background: #384250; border-color: #4a5563; }
QPushButton:pressed { background: #26508f; }
QPushButton:disabled { color: #6b7684; background: #232830; }

/* ---- Tab 页签 ---- */
QTabWidget::pane { border: 1px solid #343b46; background: #1b1f24; top: -1px; }
QTabBar::tab {
    background: #232830; color: #9aa5b1; padding: 7px 24px;
    border: 1px solid #343b46; border-bottom: none;
    border-top-left-radius: 4px; border-top-right-radius: 4px;
    margin-right: 2px;
}
QTabBar::tab:selected { background: #1b1f24; color: #4da3ff; border-top: 2px solid #3d8bff; }
QTabBar::tab:!selected { margin-top: 2px; }

/* ---- 输入/列表 ---- */
QLineEdit {
    background: #12151a; border: 1px solid #3a424e; border-radius: 4px;
    padding: 4px 8px; selection-background-color: #2f6fd0;
}
QLineEdit:focus { border-color: #3d8bff; }
QListWidget { background: #12151a; border: 1px solid #343b46; border-radius: 4px; }
QListWidget::item { padding: 7px; border-bottom: 1px solid #232830; }
QListWidget::item:selected { background: #2f6fd0; color: #ffffff; }
QListWidget::item:hover:!selected { background: #262b33; }

/* ---- 状态栏/标签 ---- */
QStatusBar { background: #232830; color: #9aa5b1; border-top: 1px solid #343b46; }
QStatusBar QLabel { color: #9aa5b1; }
QLabel { background: transparent; }

/* ---- 滚动条 ---- */
QScrollBar:vertical { background: #1b1f24; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: #3a424e; border-radius: 5px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: #4a5563; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar:horizontal { background: #1b1f24; height: 10px; margin: 0; }
QScrollBar::handle:horizontal { background: #3a424e; border-radius: 5px; min-width: 30px; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }

QMessageBox { background: #232830; }
)");

    MainWindow w;
    w.show();
    return a.exec();
}
