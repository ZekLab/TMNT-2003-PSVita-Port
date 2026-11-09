"""TMNT (PC, 2003) Vita files builder: the window of builder_app.py around tools/make_vita_files.py.

    python3 tools/builder/tmnt_builder.py [GAME_FOLDER OUT_FOLDER]   (both given: the build starts at once)
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))

import builder_app  # noqa: E402
import make_vita_files as M  # noqa: E402


def build(game, out, work, log, progress):
    M.log, M.progress = log, progress
    M.make_vita_files(game, out, work)


PORT = builder_app.Port(name="TMNT (2003)", vita_folder=M.VITA_FOLDER, out_name="TMNT2003_Vita",
                        game_hint="the folder of the installed PC game (it holds TMNT_EN.EXE)", build=build)

if __name__ == "__main__":
    args = sys.argv[1:]
    builder_app.BuilderApp(PORT).run_app(*(args if len(args) == 2 else ()))
