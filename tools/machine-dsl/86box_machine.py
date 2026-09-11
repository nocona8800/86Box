#!/usr/bin/env python3
"""86Box .86m authoring toolkit.

This is deliberately dependency-free so contributors can lint and normalize
machine descriptions with the Python already used by many build environments.
The emulator itself consumes generated/compiled IR, never this parser.
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import pathlib
import re
import sys
from typing import Iterable, Sequence


class Diagnostic(Exception):
    def __init__(self, path: pathlib.Path, line: int, column: int, message: str):
        super().__init__(message)
        self.path, self.line, self.column, self.message = path, line, column, message

    def __str__(self) -> str:
        return f"{self.path}:{self.line}:{self.column}: error: {self.message}"


@dataclasses.dataclass(frozen=True)
class Token:
    kind: str
    text: str
    line: int
    column: int


@dataclasses.dataclass
class Statement:
    head: list[Token]
    children: list["Statement"] | None = None

    @property
    def line(self) -> int:
        return self.head[0].line if self.head else 1

    def words(self) -> list[str]:
        return [t.text for t in self.head]


@dataclasses.dataclass
class Document:
    path: pathlib.Path
    schema: int
    declarations: list[Statement]
    source: str = ""


PUNCTUATION = set("{}[]=,:;().")
RUNTIME_DEVICES = {
    "ali.m1541", "ali.m1543c", "flash.sst_39sf020", "flash.sst_29ee010",
    "intel.i440bx", "intel.piix4e", "sio.w83977", "clock.ics9250_08",
    "hwm.as99127f",
    "hwm.w83781d_p5a", "intel.i430tx", "intel.piix4", "sio.pc87307",
    "flash.intel_bxt", "onboard.sound", "onboard.video", "onboard.network",
}
RUNTIME_ROLES = {
    "northbridge", "agp_bridge", "southbridge", "southbridge_ide",
    "southbridge_pmu", "southbridge_usb", "expansion", "video", "sound",
    "network", "scsi", "ide", "bridge",
}


def lex(path: pathlib.Path, source: str) -> list[Token]:
    out: list[Token] = []
    i = 0
    line = column = 1
    while i < len(source):
        ch = source[i]
        if ch in " \t\r":
            i += 1
            column += 1
            continue
        if ch == "\n":
            out.append(Token("newline", "\n", line, column))
            i += 1
            line, column = line + 1, 1
            continue
        if ch == "#":
            while i < len(source) and source[i] != "\n":
                i += 1
                column += 1
            continue
        if source.startswith("->", i) or source.startswith("..", i):
            out.append(Token("punct", source[i:i + 2], line, column))
            i += 2
            column += 2
            continue
        if source.startswith("==", i) or source.startswith("!=", i) or source.startswith("<=", i) or source.startswith(">=", i):
            out.append(Token("operator", source[i:i + 2], line, column))
            i += 2
            column += 2
            continue
        if ch in "<>":
            out.append(Token("operator", ch, line, column))
            i += 1
            column += 1
            continue
        if ch in PUNCTUATION:
            out.append(Token("punct", ch, line, column))
            i += 1
            column += 1
            continue
        if ch == '"':
            start_line, start_column = line, column
            value = ['"']
            i += 1
            column += 1
            escaped = False
            while i < len(source):
                c = source[i]
                value.append(c)
                i += 1
                column += 1
                if escaped:
                    escaped = False
                elif c == "\\":
                    escaped = True
                elif c == '"':
                    break
                elif c == "\n":
                    raise Diagnostic(path, start_line, start_column, "newline in string literal")
            else:
                raise Diagnostic(path, start_line, start_column, "unterminated string literal")
            text = "".join(value)
            try:
                json.loads(text)
            except json.JSONDecodeError as exc:
                raise Diagnostic(path, start_line, start_column, f"invalid string escape: {exc.msg}") from None
            out.append(Token("string", text, start_line, start_column))
            continue
        match = re.match(r"(?:0[xX][0-9a-fA-F]+|0[bB][01]+|[0-9]+(?:\.[0-9]+)?)", source[i:])
        if match:
            text = match.group(0)
            out.append(Token("number", text, line, column))
            i += len(text)
            column += len(text)
            continue
        match = re.match(r"\$?[A-Za-z_][A-Za-z0-9_.-]*", source[i:])
        if match:
            text = match.group(0)
            out.append(Token("ident", text, line, column))
            i += len(text)
            column += len(text)
            continue
        raise Diagnostic(path, line, column, f"unexpected character {ch!r}")
    out.append(Token("eof", "", line, column))
    return out


class Parser:
    def __init__(self, path: pathlib.Path, tokens: Sequence[Token]):
        self.path, self.tokens, self.pos = path, tokens, 0

    def skip_separators(self) -> None:
        while self.tokens[self.pos].kind == "newline" or self.tokens[self.pos].text == ";":
            self.pos += 1

    def parse(self) -> Document:
        self.skip_separators()
        schema = 0
        if self.tokens[self.pos].text == "schema":
            start = self.tokens[self.pos]
            self.pos += 1
            if self.tokens[self.pos].kind != "number":
                self.fail(self.tokens[self.pos], "expected schema version")
            schema = int(self.tokens[self.pos].text, 0)
            self.pos += 1
            if schema != 1:
                self.fail(start, f"unsupported schema {schema}; expected 1")
            self.consume_line_end()
        else:
            self.fail(self.tokens[self.pos], "file must begin with 'schema 1'")
        declarations = self.parse_statements(top_level=True)
        return Document(self.path, schema, declarations)

    def consume_line_end(self) -> None:
        if self.tokens[self.pos].kind not in ("newline", "eof") and self.tokens[self.pos].text != ";":
            self.fail(self.tokens[self.pos], "expected end of statement")
        self.skip_separators()

    def parse_statements(self, top_level: bool = False) -> list[Statement]:
        result: list[Statement] = []
        self.skip_separators()
        while self.tokens[self.pos].kind != "eof" and self.tokens[self.pos].text != "}":
            head: list[Token] = []
            square_depth = 0
            while True:
                tok = self.tokens[self.pos]
                if tok.kind == "eof":
                    break
                if tok.kind == "newline" and square_depth == 0:
                    break
                if tok.text == ";" and square_depth == 0:
                    break
                if tok.text == "{" and square_depth == 0:
                    break
                if tok.text == "}" and square_depth == 0:
                    break
                if tok.text == "[":
                    square_depth += 1
                elif tok.text == "]":
                    square_depth -= 1
                    if square_depth < 0:
                        self.fail(tok, "unmatched ']'")
                head.append(tok)
                self.pos += 1
            if square_depth:
                self.fail(head[-1], "unterminated list")
            if not head:
                self.fail(self.tokens[self.pos], "expected statement")
            children = None
            if self.tokens[self.pos].text == "{":
                self.pos += 1
                children = self.parse_statements()
                if self.tokens[self.pos].text != "}":
                    self.fail(self.tokens[self.pos], "unterminated block")
                self.pos += 1
            result.append(Statement(head, children))
            self.skip_separators()
        if top_level and self.tokens[self.pos].text == "}":
            self.fail(self.tokens[self.pos], "unmatched '}'")
        return result

    def fail(self, token: Token, message: str) -> None:
        raise Diagnostic(self.path, token.line, token.column, message)


def parse_file(path: pathlib.Path) -> Document:
    source = path.read_text(encoding="utf-8")
    document = Parser(path, lex(path, source)).parse()
    document.source = source
    return document


def declaration_identity(stmt: Statement) -> tuple[str, str] | None:
    words = stmt.words()
    if not words or words[0] not in ("profile", "machine", "gpio", "sequence", "resource") or len(words) < 2:
        return None
    name = words[1]
    if name.startswith('"'):
        name = json.loads(name)
    return words[0], name


def flatten(statements: Iterable[Statement]) -> Iterable[Statement]:
    for statement in statements:
        yield statement
        if statement.children is not None:
            yield from flatten(statement.children)


def validate(documents: Sequence[Document]) -> list[str]:
    errors: list[str] = []
    declarations: dict[tuple[str, str], tuple[Document, Statement]] = {}
    profiles: set[str] = set()
    for document in documents:
        for declaration in document.declarations:
            identity = declaration_identity(declaration)
            if identity is None:
                errors.append(f"{document.path}:{declaration.line}:1: error: unsupported top-level declaration")
                continue
            if declaration.children is None:
                errors.append(f"{document.path}:{declaration.line}:1: error: declaration requires a block")
            if identity in declarations:
                prior_doc, prior = declarations[identity]
                errors.append(f"{document.path}:{declaration.line}:1: error: duplicate {identity[0]} '{identity[1]}'; first declared at {prior_doc.path}:{prior.line}")
            declarations[identity] = (document, declaration)
            if identity[0] == "profile":
                profiles.add(identity[1])

    for document in documents:
        for declaration in document.declarations:
            words = declaration.words()
            identity = declaration_identity(declaration)
            if not identity:
                continue
            if "extends" in words:
                index = words.index("extends")
                missing_base = index + 1 >= len(words)
                external_machine_base = identity[0] == "machine" and not missing_base
                if missing_base or (not external_machine_base and words[index + 1].strip('"') not in profiles):
                    errors.append(f"{document.path}:{declaration.line}:1: error: unknown base profile after 'extends'")
            if identity[0] == "machine" and declaration.children is not None:
                has_name = any(s.words()[:2] == ["name", "="] for s in declaration.children)
                if not has_name:
                    errors.append(f"{document.path}:{declaration.line}:1: error: machine '{identity[1]}' has no display name")
            for statement in flatten(declaration.children or []):
                line = statement.words()
                if line[:2] == ["file", "="] and len(line) >= 3:
                    value = json.loads(line[2]) if line[2].startswith('"') else line[2]
                    candidate = pathlib.PurePosixPath(value)
                    if candidate.is_absolute() or ".." in candidate.parts:
                        errors.append(f"{document.path}:{statement.line}:1: error: firmware path must be relative and may not contain '..'")
                if line and line[0] == "slot" and "route" in line:
                    ri = line.index("route")
                    route = [x for x in line[ri + 1:] if x not in ("[", "]", ",")]
                    if len(route) != 4:
                        errors.append(f"{document.path}:{statement.line}:1: error: PCI route requires exactly four entries")
                if line and line[0] == "native" and statement.children is not None:
                    has_reason = any(s.words()[:2] == ["reason", "="] for s in statement.children)
                    if not has_reason:
                        errors.append(f"{document.path}:{statement.line}:1: error: native blocks require a reason")
                if line == ["runtime"] and statement.children is not None:
                    errors.extend(validate_runtime(document.path, statement.children))
    return errors


def validate_runtime(path: pathlib.Path, statements: Sequence[Statement]) -> list[str]:
    """Validate the deliberately flat grammar implemented by machine_dsl.c."""
    errors: list[str] = []
    saw_firmware = saw_platform = saw_pci = False
    for statement in statements:
        words = [word for word in statement.words() if word not in ("[", "]", ",")]
        reason = None
        if statement.children is not None:
            reason = "runtime statements cannot contain nested blocks"
        elif words[:2] == ["firmware", "linear"] and len(words) == 6:
            saw_firmware = True
        elif words == ["platform", "at.common"]:
            saw_platform = True
        elif words == ["pci", "config_type_1"]:
            saw_pci = True
        elif words and words[0] == "slot":
            if not saw_pci:
                reason = "PCI slot appears before PCI initialization"
            elif len(words) != 8 or words[2] not in RUNTIME_ROLES or words[3] != "route":
                reason = "expected 'slot HEX ROLE route [A, B, C, D]'"
            elif any(route not in {"none", "A", "B", "C", "D", "E", "F", "G", "H"} for route in words[4:]):
                reason = "invalid PCI interrupt route"
        elif words[:2] == ["spd", "sdram"] and len(words) == 4:
            pass
        elif words[:2] in (["gpio", "default"], ["gpio", "acpi_default"]) and len(words) == 3:
            pass
        elif words and words[0] == "device":
            if len(words) < 2 or words[1] not in RUNTIME_DEVICES:
                reason = "unknown runtime device id"
            else:
                tail = words[2:]
                while tail and reason is None:
                    if tail[0] == "params" and len(tail) >= 2:
                        tail = tail[2:]
                    elif tail[0] == "when" and len(tail) >= 2 and tail[1] in {"sound.internal", "video.internal", "network.internal"}:
                        tail = tail[2:]
                    elif tail[0] == "capture" and len(tail) >= 2 and tail[1] == "machine_snd":
                        tail = tail[2:]
                    else:
                        reason = "invalid device modifier"
        else:
            reason = "unsupported normalized runtime statement"
        if reason:
            errors.append(f"{path}:{statement.line}:1: error: {reason}")
    if not saw_firmware:
        errors.append(f"{path}:1:1: error: runtime block has no firmware")
    if not saw_platform:
        errors.append(f"{path}:1:1: error: runtime block has no platform")
    return errors


def token_text(tokens: Sequence[Token]) -> str:
    result = ""
    previous = ""
    no_space_before = {",", "]", ":"}
    no_space_after = {"["}
    for token in tokens:
        text = token.text
        if result and text not in no_space_before and previous not in no_space_after and text != "=" and previous != "=":
            result += " "
        if text == "=":
            result = result.rstrip() + " = "
        else:
            result += text
        previous = text
    return result.rstrip()


def format_statements(statements: Sequence[Statement], indent: int = 0) -> list[str]:
    lines: list[str] = []
    for statement in statements:
        prefix = "    " * indent + token_text(statement.head)
        if statement.children is None:
            lines.append(prefix)
        else:
            lines.append(prefix + " {")
            lines.extend(format_statements(statement.children, indent + 1))
            lines.append("    " * indent + "}")
    return lines


def format_document(document: Document) -> str:
    # Comments are intentionally not AST nodes: formatting is therefore a
    # conservative whitespace pass which cannot discard or relocate them.
    return "\n".join(line.rstrip() for line in document.source.splitlines()).rstrip() + "\n"


def statement_json(statement: Statement) -> dict:
    value = {"head": statement.words(), "line": statement.line}
    if statement.children is not None:
        value["children"] = [statement_json(child) for child in statement.children]
    return value


def document_json(document: Document) -> dict:
    return {
        "schema": document.schema,
        "source": str(document.path),
        "declarations": [statement_json(s) for s in document.declarations],
    }


def load(paths: Sequence[str]) -> list[Document]:
    documents: list[Document] = []
    for raw in paths:
        path = pathlib.Path(raw)
        if path.is_dir():
            for child in sorted(path.rglob("*.86m")):
                documents.append(parse_file(child))
        else:
            documents.append(parse_file(path))
    return documents


def cmd_check(args: argparse.Namespace) -> int:
    documents = load(args.paths)
    errors = validate(documents)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"checked {len(documents)} file(s), {sum(len(d.declarations) for d in documents)} declaration(s)")
    return 0


def cmd_fmt(args: argparse.Namespace) -> int:
    documents = load(args.paths)
    if args.check:
        changed = [str(d.path) for d in documents if d.path.read_text(encoding="utf-8") != format_document(d)]
        if changed:
            print("would reformat:\n" + "\n".join(changed), file=sys.stderr)
            return 1
        return 0
    for document in documents:
        rendered = format_document(document)
        if args.stdout:
            sys.stdout.write(rendered)
        else:
            document.path.write_text(rendered, encoding="utf-8", newline="\n")
    return 0


def cmd_compile(args: argparse.Namespace) -> int:
    documents = load(args.paths)
    errors = validate(documents)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    payload = {
        "format": "86box-machine-ir",
        "abi": 1,
        "documents": [document_json(document) for document in documents],
    }
    output = json.dumps(payload, indent=2, sort_keys=True) + "\n"
    if args.output:
        pathlib.Path(args.output).write_text(output, encoding="utf-8", newline="\n")
    else:
        sys.stdout.write(output)
    return 0


def cmd_explain(args: argparse.Namespace) -> int:
    documents = load(args.paths)
    errors = validate(documents)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    for document in documents:
        for declaration in document.declarations:
            identity = declaration_identity(declaration)
            if identity and identity[1] == args.name:
                print(f"{identity[0]} {identity[1]} ({document.path}:{declaration.line})")
                print("\n".join(format_statements([declaration])))
                return 0
    print(f"machine/profile/gpio '{args.name}' not found", file=sys.stderr)
    return 1


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="86box-machine", description="86Box .86m authoring toolkit")
    sub = parser.add_subparsers(dest="command", required=True)
    check = sub.add_parser("check", help="parse and validate descriptions")
    check.add_argument("paths", nargs="+")
    check.set_defaults(func=cmd_check)
    fmt = sub.add_parser("fmt", help="safely normalize trailing whitespace and final newline")
    fmt.add_argument("paths", nargs="+")
    fmt.add_argument("--check", action="store_true")
    fmt.add_argument("--stdout", action="store_true")
    fmt.set_defaults(func=cmd_fmt)
    compile_cmd = sub.add_parser("compile", help="emit versioned JSON IR for tooling")
    compile_cmd.add_argument("paths", nargs="+")
    compile_cmd.add_argument("-o", "--output")
    compile_cmd.set_defaults(func=cmd_compile)
    explain = sub.add_parser("explain", help="show one resolved source declaration")
    explain.add_argument("name")
    explain.add_argument("paths", nargs="+")
    explain.set_defaults(func=cmd_explain)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = make_parser().parse_args(argv)
        return args.func(args)
    except (Diagnostic, OSError, ValueError) as exc:
        print(exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
