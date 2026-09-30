#!/usr/bin/env python3
"""Main water meter readings from the utility's invoices (PDF), for checking the telemetry against the meter.

  uv run --with pypdf server/model/invoices.py ~/path/to/invoices

Writes server/model/meter_readings.csv (gitignored): start date, reading, end date, reading, in m³. Invoices are
private documents: only these reading rows are kept. No amounts, names, addresses, account or meter numbers are
read into the output, and sub-meter rows are skipped (the sub-meter sits behind the flow meter and adds nothing).
The main meter is recognised by the invoice wording (radio reading, "licznik nadrzędny"), not by its number.
"""
import argparse
import csv
import pathlib
import re

HERE = pathlib.Path(__file__).resolve().parent
OUT = HERE / "meter_readings.csv"

DATE = r"(\d{4}-\d{2}-\d{2})"
NUM = r"([\d.]+)"
# Wordings seen on the invoices: 2024+ ("odcz. poprz.: ... bież.: ..."), 2023 ("Odcz.poprz: ... Bieżący: ..."), and
# the main-meter line printed next to a sub-meter ("licznika nadrzędnego ...: poprz. ... bież. ...")
PATTERNS = [
    re.compile(rf"Odczyt radiowy \d+ -? ?odcz\. poprz\.: {DATE} {NUM} ?m3, bie[żz]\.: {DATE} {NUM} ?m3"),
    re.compile(rf"Odczyt radiowy \d+ Odcz\.poprz: {DATE} {NUM} ?m3 Bie[żz][aą]cy: {DATE} {NUM} ?m3"),
    re.compile(rf"licznika nadrz[eę]dnego(?: nr \d+)?: poprz\. {DATE} {NUM} ?m3, bie[żz]\. {DATE} {NUM} ?m3"),
]


def parse_text(text):
    """Set of (start date, start m³, end date, end m³) main-meter intervals found in one document's text."""
    t = re.sub(r"\s+", " ", text)
    found = set()
    for p in PATTERNS:
        for a, x, b, y in p.findall(t):
            found.add((a, float(x), b, float(y)))
    return found


def read_dir(path):
    from pypdf import PdfReader
    rows = set()
    for f in sorted(p for p in pathlib.Path(path).iterdir() if p.suffix.lower() == ".pdf"):
        try:
            text = " ".join((page.extract_text() or "") for page in PdfReader(f).pages)
        except Exception as e:  # a damaged or unrelated PDF must not stop the rest
            print(f"skipped {f.name}: {e}")
            continue
        rows |= parse_text(text)
    return sorted(rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dir", help="directory with the invoice PDFs")
    ap.add_argument("--out", type=pathlib.Path, default=OUT)
    args = ap.parse_args()
    rows = read_dir(args.dir)
    with open(args.out, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["start_local", "start_m3", "end_local", "end_m3"])
        w.writerows(rows)
    gaps = [(a[2], b[0]) for a, b in zip(rows, rows[1:]) if a[2] != b[0]]
    print(f"{len(rows)} main-meter intervals, {rows[0][0] if rows else '-'} .. {rows[-1][2] if rows else '-'}"
          f"{'; not consecutive at ' + str(gaps) if gaps else ''} -> {args.out}")


if __name__ == "__main__":
    main()
