#!/usr/bin/env python3
import argparse, datetime as dt, json, os, re, subprocess, tempfile
from openpyxl import load_workbook


def run(cmd):
    return subprocess.check_output(cmd, stderr=subprocess.STDOUT, text=True)


def ocr_pdf(pdf_path: str) -> str:
    with tempfile.TemporaryDirectory() as td:
        base = os.path.join(td, "page")
        run(["pdftoppm", "-png", "-r", "300", pdf_path, base])
        img = f"{base}-1.png"
        texts = []
        for rot in [0, 90, 180, 270]:
            rimg = os.path.join(td, f"r{rot}.png")
            run(["python3", "-c", (
                "from PIL import Image; "
                f"Image.open('{img}').rotate({rot}, expand=True).save('{rimg}')"
            )])
            out = os.path.join(td, f"ocr_{rot}")
            subprocess.run(["tesseract", rimg, out, "-l", "jpn", "--oem", "1", "--psm", "6"], check=False)
            txt_path = out + ".txt"
            txt = open(txt_path, encoding="utf-8", errors="ignore").read() if os.path.exists(txt_path) else ""
            score = len(re.findall(r"[ぁ-んァ-ヶ一-龥]", txt))
            texts.append((score, rot, txt))
        texts.sort(reverse=True)
        return texts[0][2]


def apply_rules(text: str, rules_path: str) -> str:
    if not rules_path or not os.path.exists(rules_path):
        return text
    rules = json.load(open(rules_path, encoding="utf-8"))
    for k, v in rules.get("replacements", {}).items():
        text = text.replace(k, v)
    return text


def pick(patterns, text):
    for p in patterns:
        m = re.search(p, text, re.MULTILINE)
        if m:
            return m.group(1).strip()
    return None


def parse_fields(text: str, pdf_name: str):
    date = None
    m = re.search(r"(20\d{2})[^\d]?(\d{1,2})[^\d]?(\d{1,2})", pdf_name)
    if m:
        date = dt.datetime(int(m.group(1)), int(m.group(2)), int(m.group(3)))

    equip_no = pick([
        r"設備No\s*[:：]?\s*([A-Za-z0-9\-]+)",
        r"設備番号\s*[:：]?\s*([A-Za-z0-9\-]+)"
    ], text)
    m2 = re.search(r"(NCD|NCMC|NCP|NC)[\-\s]?(\d{3})", pdf_name, re.IGNORECASE)
    if m2:
        equip_no = f"{m2.group(1).upper()}-{m2.group(2)}"

    symptom = pick([
        r"症状\s*[:：]\s*(.+)",
        r"症状[^\n]*\n(.+)"
    ], text)
    cause = pick([
        r"原因\s*[:：]\s*(.+)",
        r"原因[^\n]*\n(.+)"
    ], text)
    action = pick([
        r"処置\s*[:：]\s*(.+)",
        r"処置[^\n]*\n(.+)"
    ], text)

    return {
        "date": date,
        "equip_no": equip_no,
        "symptom": symptom,
        "cause": cause,
        "action": action
    }


def append_excel(xlsx_path: str, fields: dict, source_pdf: str):
    wb = load_workbook(xlsx_path)
    ws = wb[wb.sheetnames[0]]
    equip_prefix = None
    if fields["equip_no"] and "-" in fields["equip_no"]:
        equip_prefix = fields["equip_no"].split("-")[0]

    row = [
        fields["date"],            # 年月日
        None,                       # AL
        '-',                        # -□
        '-',                        # 記号
        '修理報告書OCR取込',         # アラーム内容/どこが
        None,                       # ｺｰﾄﾞ/軸
        None,                       # 発生時
        fields["symptom"],         # 症状/どう
        fields["cause"],           # 原因/部位
        fields["action"],          # 処置
        None,                       # 部品型番
        None,                       # メーカー
        equip_prefix,               # 設備
        fields["equip_no"],        # 設備番号
        None,                       # 設備型式
        None,                       # 制御装置
        f"元資料: {os.path.basename(source_pdf)} / OCR自動追記",
        None
    ]
    ws.append(row)
    wb.save(xlsx_path)
    return ws.max_row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pdf", required=True)
    ap.add_argument("--xlsx", required=True)
    ap.add_argument("--rules", default="")
    ap.add_argument("--print-only", action="store_true")
    args = ap.parse_args()

    txt = ocr_pdf(args.pdf)
    txt = apply_rules(txt, args.rules)
    fields = parse_fields(txt, os.path.basename(args.pdf))

    print("[OCR EXTRACT]")
    for k, v in fields.items():
        print(f"- {k}: {v}")

    if args.print_only:
        return

    row = append_excel(args.xlsx, fields, args.pdf)
    print(f"[OK] appended row={row} to {args.xlsx}")


if __name__ == "__main__":
    main()
