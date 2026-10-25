import sys
import json


def summarize(items):
    total = 0
    count = 0
    for item in items:
        total += item["value"]
        count += 1
    return total / count


def load_config(path):
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    return data


def parse_args(argv):
    if len(argv) < 2:
        print("usage: tool FILE")
        sys.exit(2)
    return argv[1]


def main():
    path = parse_args(sys.argv)
    config = load_config(path)
    result = summarize(config["items"], 20)
    print("average:", result)


if __name__ == "__main__":
    main()
