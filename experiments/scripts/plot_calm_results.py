#!/usr/bin/env python3
"""Generate publication-friendly CALM comparison plots from curated CSV data."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter


DEFAULT_COLOR = "#C45145"
CALM_COLOR = "#087E6A"
GRID_COLOR = "#D9DEE3"


def load_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def improvement(row: dict[str, str]) -> str:
    default = float(row["default"])
    calm = float(row["calm"])
    if row["better"] == "higher":
        return f"+{calm - default:.1f} pp"
    return f"{(calm / default - 1.0) * 100.0:.1f}%"


def value_label(value: float, unit: str) -> str:
    if unit == "%":
        return f"{value:.1f}%"
    if unit == "count":
        return f"{value:g}"
    if value >= 100:
        return f"{value:.1f}"
    return f"{value:.2f}"


def style_axis(axis: plt.Axes) -> None:
    axis.spines[["top", "right"]].set_visible(False)
    axis.grid(axis="y", color=GRID_COLOR, linewidth=0.8)
    axis.set_axisbelow(True)
    axis.tick_params(axis="both", labelsize=9)


def plot_metric_bars(rows: list[dict[str, str]], output: Path) -> None:
    fig, axes = plt.subplots(2, 3, figsize=(13.5, 7.8), constrained_layout=True)
    fig.suptitle(
        "Fast DDS: Optimized Default vs CALM 4.0 (fixed $T_p$ = 50 ms)",
        fontsize=16,
        fontweight="bold",
    )
    for axis, row in zip(axes.flat, rows):
        values = [float(row["default"]), float(row["calm"])]
        bars = axis.bar(
            ["Optimized\nDefault", "CALM 4.0"],
            values,
            color=[DEFAULT_COLOR, CALM_COLOR],
            width=0.58,
        )
        axis.set_title(row["label"], fontsize=11, fontweight="bold")
        axis.set_ylabel(row["unit"])
        axis.set_ylim(0, max(values) * 1.27)
        style_axis(axis)
        for bar, value in zip(bars, values):
            axis.text(
                bar.get_x() + bar.get_width() / 2,
                bar.get_height() + max(values) * 0.035,
                value_label(value, row["unit"]),
                ha="center",
                va="bottom",
                fontsize=9,
                fontweight="bold",
            )
        axis.text(
            0.98,
            0.95,
            improvement(row),
            transform=axis.transAxes,
            ha="right",
            va="top",
            color=CALM_COLOR,
            fontsize=10,
            fontweight="bold",
        )
    fig.text(
        0.5,
        -0.015,
        "1 MiB x 20 Hz, 2,000 samples, 180 Mbps loopback, 3 +/- 1 ms, persistent PER 10%, OPT 1+2",
        ha="center",
        fontsize=9,
        color="#4B5563",
    )
    fig.savefig(output, dpi=180, bbox_inches="tight", facecolor="white")
    plt.close(fig)


def plot_metric_lines(rows: list[dict[str, str]], output: Path) -> None:
    fig, axes = plt.subplots(2, 3, figsize=(13.5, 7.8), constrained_layout=True)
    fig.suptitle(
        "Metric change from Optimized Default to CALM 4.0",
        fontsize=16,
        fontweight="bold",
    )
    for axis, row in zip(axes.flat, rows):
        values = [float(row["default"]), float(row["calm"])]
        axis.plot([0, 1], values, color="#52606D", linewidth=2.2, zorder=2)
        axis.scatter([0], [values[0]], s=95, color=DEFAULT_COLOR, zorder=3)
        axis.scatter([1], [values[1]], s=95, color=CALM_COLOR, zorder=3)
        axis.set_xticks([0, 1], ["Optimized\nDefault", "CALM 4.0"])
        axis.set_xlim(-0.25, 1.25)
        low = min(0.0, min(values) * 0.85)
        high = max(values) * 1.20
        axis.set_ylim(low, high)
        axis.set_title(row["label"], fontsize=11, fontweight="bold")
        axis.set_ylabel(row["unit"])
        style_axis(axis)
        for x, value, color in zip([0, 1], values, [DEFAULT_COLOR, CALM_COLOR]):
            axis.annotate(
                value_label(value, row["unit"]),
                (x, value),
                xytext=(0, 9),
                textcoords="offset points",
                ha="center",
                color=color,
                fontsize=9,
                fontweight="bold",
            )
        axis.text(
            0.98,
            0.95,
            improvement(row),
            transform=axis.transAxes,
            ha="right",
            va="top",
            color=CALM_COLOR,
            fontsize=10,
            fontweight="bold",
        )
    fig.text(
        0.5,
        -0.015,
        "Lower is better except receive ratio. The Default run timed out at 300 seconds.",
        ha="center",
        fontsize=9,
        color="#4B5563",
    )
    fig.savefig(output, dpi=180, bbox_inches="tight", facecolor="white")
    plt.close(fig)


def plot_cross_condition_bars(rows: list[dict[str, str]], output: Path) -> None:
    labels = [row["label"].replace(" x ", "\nx ") for row in rows]
    default_values = [float(row["default_p95_s"]) for row in rows]
    calm_values = [float(row["calm_p95_s"]) for row in rows]
    x_positions = range(len(rows))
    width = 0.35

    fig, axis = plt.subplots(figsize=(12.5, 6.6))
    fig.subplots_adjust(bottom=0.20, left=0.09, right=0.98, top=0.90)
    default_bars = axis.bar(
        [x - width / 2 for x in x_positions],
        default_values,
        width,
        color=DEFAULT_COLOR,
        label="Optimized Default",
    )
    calm_bars = axis.bar(
        [x + width / 2 for x in x_positions],
        calm_values,
        width,
        color=CALM_COLOR,
        label="CALM",
    )
    axis.set_yscale("log")
    axis.set_ylabel("p95 end-to-end delay (seconds, log scale)")
    axis.set_title("Documented CALM comparisons across middleware and workloads", fontweight="bold")
    axis.set_xticks(list(x_positions), labels)
    axis.legend(frameon=False, loc="upper right")
    style_axis(axis)
    axis.yaxis.set_major_formatter(FuncFormatter(lambda value, _: f"{value:g}"))
    for bars, values in ((default_bars, default_values), (calm_bars, calm_values)):
        for bar, value in zip(bars, values):
            axis.annotate(
                f"{value:.3g}s",
                (bar.get_x() + bar.get_width() / 2, value),
                xytext=(0, 5),
                textcoords="offset points",
                ha="center",
                fontsize=9,
            )
    fig.text(
        0.5,
        0.035,
        "Fast: CALM 4.0 fixed 50 ms (1 run). Cyclone: CALM 3 prototype (mean of 2 runs).",
        ha="center",
        fontsize=9,
        color="#4B5563",
    )
    fig.savefig(output, dpi=180, bbox_inches="tight", facecolor="white")
    plt.close(fig)


def plot_cross_condition_lines(rows: list[dict[str, str]], output: Path) -> None:
    palette = ["#1768AC", "#B46A00", "#6B4C9A"]
    fig, axis = plt.subplots(figsize=(10.5, 6.6))
    fig.subplots_adjust(bottom=0.18, left=0.11, right=0.98, top=0.90)
    for row, color in zip(rows, palette):
        values = [float(row["default_p95_s"]), float(row["calm_p95_s"])]
        axis.plot(
            [0, 1],
            values,
            marker="o",
            markersize=8,
            linewidth=2.3,
            color=color,
            label=row["label"],
        )
        for x, value in enumerate(values):
            axis.annotate(
                f"{value:.3g}s",
                (x, value),
                xytext=(0, 7),
                textcoords="offset points",
                ha="center",
                color=color,
                fontsize=9,
                fontweight="bold",
            )
    axis.set_yscale("log")
    axis.set_xlim(-0.1, 1.1)
    axis.set_xticks([0, 1], ["Optimized Default", "CALM"])
    axis.set_ylabel("p95 end-to-end delay (seconds, log scale)")
    axis.set_title("p95 delay change by controlled comparison", fontweight="bold")
    axis.legend(frameon=False, loc="best", fontsize=9)
    style_axis(axis)
    axis.yaxis.set_major_formatter(FuncFormatter(lambda value, _: f"{value:g}"))
    fig.text(
        0.5,
        0.035,
        "Controller versions differ by series; see the CSV and README before cross-study interpretation.",
        ha="center",
        fontsize=9,
        color="#4B5563",
    )
    fig.savefig(output, dpi=180, bbox_inches="tight", facecolor="white")
    plt.close(fig)


def parse_args() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--data-dir",
        type=Path,
        default=repository / "docs" / "data",
        help="Directory containing the curated comparison CSV files",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=repository / "docs" / "assets",
        help="Directory for generated PNG files",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    metric_rows = load_rows(args.data_dir / "tp50_fastdds_metrics.csv")
    cross_rows = load_rows(args.data_dir / "cross_condition_p95.csv")

    plot_metric_bars(metric_rows, args.output_dir / "tp50_metrics_bar.png")
    plot_metric_lines(metric_rows, args.output_dir / "tp50_metrics_line.png")
    plot_cross_condition_bars(cross_rows, args.output_dir / "cross_condition_p95_bar.png")
    plot_cross_condition_lines(cross_rows, args.output_dir / "cross_condition_p95_line.png")

    for path in sorted(args.output_dir.glob("*.png")):
        print(path)


if __name__ == "__main__":
    main()
