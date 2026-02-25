"""Launch the SeqEyes GUI viewer from Python."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

_exe_name = "seqeyes.exe" if sys.platform == "win32" else "seqeyes"
# Bundled binary installed by the Python wheel alongside this module.
_BUNDLED_EXE = Path(__file__).parent / "bin" / _exe_name


def _find_executable() -> str:
    """Return the path to the seqeyes executable.

    Checks (in order):
    1. Bundled binary shipped inside this Python package (``seqeyes/bin/``).
    2. ``seqeyes`` on the system ``PATH``.
    """
    if _BUNDLED_EXE.is_file():
        return str(_BUNDLED_EXE)

    exe = shutil.which("seqeyes")
    if exe is None:
        raise FileNotFoundError(
            "SeqEyes executable not found. "
            "Install a binary wheel via 'pip install seqeyes', "
            "or add seqeyes to your PATH."
        )
    return exe


def seqeyes(*args) -> None:
    """Launch the SeqEyes GUI viewer.

    Mirrors the MATLAB ``seqeyes()`` wrapper.  All preceding positional
    arguments are passed as command-line options; the last positional
    argument is the sequence source.

    Parameters
    ----------
    *args :
        Options followed by a sequence source.  The source may be:

        - a ``str`` or :class:`os.PathLike` path to a ``.seq`` file,
        - an object with a ``write(filepath)`` method (e.g. a
          `pypulseq <https://github.com/imr-framework/pypulseq>`_ sequence
          object), or
        - an option string starting with ``-`` (options-only call, e.g.
          ``seqeyes.seqeyes('--help')``).

        If called with no arguments the SeqEyes GUI opens with no file loaded.

    Examples
    --------
    Open a ``.seq`` file:

    >>> import seqeyes
    >>> seqeyes.seqeyes('path/to/sequence.seq')

    Open a pypulseq in-memory sequence:

    >>> seqeyes.seqeyes(seq)

    Pass extra CLI options before the source:

    >>> seqeyes.seqeyes('--layout', '212', 'path/to/sequence.seq')

    Raises
    ------
    FileNotFoundError
        If the ``seqeyes`` executable cannot be found on ``PATH``.
    FileNotFoundError
        If a ``.seq`` filepath is given but the file does not exist.
    TypeError
        If the last argument is not a recognised sequence source.
    """
    exe = _find_executable()
    cmd = [exe]

    if not args:
        # No-argument call: open GUI with no file loaded
        subprocess.Popen(cmd)
        return

    last = args[-1]
    options = list(args[:-1])
    seq_fn = None
    _tmp_path = None  # keep tempfile path for reference (delete=False)

    if isinstance(last, str) and last.startswith("-"):
        # Options-only call (e.g. '--help')
        options = list(args)
        last = None
    elif isinstance(last, (str, os.PathLike)):
        seq_fn = os.fspath(last)
        if not os.path.isfile(seq_fn):
            raise FileNotFoundError(f"Seq file not found: {seq_fn}")
    elif hasattr(last, "write"):
        # Sequence object (e.g. pypulseq.Sequence) – write to a temp file
        tmp = tempfile.NamedTemporaryFile(suffix=".seq", delete=False)
        tmp.close()
        _tmp_path = tmp.name
        seq_fn = _tmp_path
        last.write(seq_fn)
    else:
        raise TypeError(
            "Last argument must be a .seq filepath, a sequence object with a "
            f"write() method, or an option string. Got: {type(last)!r}"
        )

    cmd.extend(options)
    if seq_fn:
        cmd.append(seq_fn)

    subprocess.Popen(cmd)
