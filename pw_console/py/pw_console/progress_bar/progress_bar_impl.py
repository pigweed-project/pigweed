# Copyright 2021 The Pigweed Authors
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.
"""Pigweed Console ProgressBar implementation.

Designed to be embedded in an existing prompt_toolkit full screen
application."""

from __future__ import annotations

from datetime import datetime, timedelta
import functools
from typing import (
    Iterable,
    Sequence,
)

from prompt_toolkit.filters import Condition
from prompt_toolkit.formatted_text import AnyFormattedText
from prompt_toolkit.layout import (
    ConditionalContainer,
    FormattedTextControl,
    HSplit,
    VSplit,
    Window,
)
from prompt_toolkit.layout.dimension import AnyDimension, D
from prompt_toolkit.styles import BaseStyle

from prompt_toolkit.shortcuts.progress_bar import (
    ProgressBar,
    ProgressBarCounter,
)
from prompt_toolkit.shortcuts.progress_bar.base import _ProgressControl
from prompt_toolkit.shortcuts.progress_bar.formatters import (
    Formatter,
    IterationsPerSecond,
    Text,
    TimeLeft,
    create_default_formatters,
)


class TextIfNotHidden(Text):
    def format(
        self,
        progress_bar: ProgressBar,
        progress: ProgressBarCounter[object],
        width: int,
    ) -> AnyFormattedText:
        if hasattr(progress, 'hide_eta') and progress.hide_eta:  # type: ignore
            return [('', ' ' * width)]
        return super().format(progress_bar, progress, width)


class IterationsPerSecondIfNotHidden(IterationsPerSecond):
    def format(
        self,
        progress_bar: ProgressBar,
        progress: ProgressBarCounter[object],
        width: int,
    ) -> AnyFormattedText:
        if hasattr(progress, 'hide_eta') and progress.hide_eta:  # type: ignore
            return [('class:iterations-per-second', ' ' * width)]
        return super().format(progress_bar, progress, width)


class TimeLeftIfNotHidden(TimeLeft):
    def format(
        self,
        progress_bar: ProgressBar,
        progress: ProgressBarCounter[object],
        width: int,
    ) -> AnyFormattedText:
        if hasattr(progress, 'hide_eta') and progress.hide_eta:  # type: ignore
            return [('class:time-left', ' ' * width)]
        return super().format(progress_bar, progress, width)


class ProgressBarImpl:
    """ProgressBar for rendering in an existing prompt_toolkit application."""

    def __init__(
        self,
        title: AnyFormattedText = None,
        formatters: Sequence[Formatter] | None = None,
        style: BaseStyle | None = None,
    ) -> None:
        self.title = title
        self.formatters = formatters or create_default_formatters()
        self.counters: list[ProgressBarCounter[object]] = []
        self.style = style

        # Create UI Application.
        title_toolbar = ConditionalContainer(
            Window(
                FormattedTextControl(lambda: self.title),
                height=1,
                style='class:progressbar,title',
            ),
            filter=Condition(lambda: self.title is not None),
        )

        def width_for_formatter(formatter: Formatter) -> AnyDimension:
            # Needs to be passed as callable (partial) to the 'width'
            # parameter, because we want to call it on every resize.
            return formatter.get_width(progress_bar=self)  # type: ignore

        progress_controls = [
            Window(
                content=_ProgressControl(self, f, None),  # type: ignore
                width=functools.partial(width_for_formatter, f),
            )
            for f in self.formatters
        ]

        self.container = HSplit(
            [
                title_toolbar,
                VSplit(
                    progress_controls,
                    height=lambda: D(
                        min=len(self.counters), max=len(self.counters)
                    ),
                ),
            ]
        )

    def __pt_container__(self):
        return self.container

    def __exit__(self, *a: object) -> None:
        pass

    def __call__(
        self,
        data: Iterable | None = None,
        label: AnyFormattedText = '',
        remove_when_done: bool = False,
        total: int | None = None,
    ) -> ProgressBarCounter:
        """
        Start a new counter.

        :param label: Title text or description for this progress. (This can be
            formatted text as well).
        :param remove_when_done: When `True`, hide this progress bar.
        :param total: Specify the maximum value if it can't be calculated by
            calling ``len``.
        """
        counter = ProgressBarCounter(
            self,  # type: ignore
            data,
            label=label,
            remove_when_done=remove_when_done,
            total=total,
        )
        # Ensure the elapsed time for the progress counter isn't ever
        # zero by making the start time one second in the past.
        counter.start_time = datetime.now() + timedelta(seconds=-1)
        self.counters.append(counter)
        return counter
