# Copyright (c) 2025 Rivos, Inc.
# SPDX-License-Identifier: Apache2

"""Safe expression evaluation.

   :author: Emmanuel Blot <eblot@rivosinc.com>
"""

from typing import Any

import ast
import operator


def safe_eval(value: str, parameters: dict[str, int]) -> int:
    """Evaluate a basic arithmetic expression using known parameters.

        This is much safer than default Python's eval/expr functions, which
        could execute any untrusted code.

        Here, only known variables are replaced and basic arithmetic
        operations are allowed.

        :param value: the value to evaluate
        :param parameters: a map a key-val named integer values
        :return: the evaluated integer
        :raise ValueError: for any unsupported syntax.
    """
    def _eval(node: ast.AST) -> Any:
        if isinstance(node, ast.Expression):
            return _eval(node.body)

        # Numeric literal (Python 3.8+: ast.Constant)
        if isinstance(node, ast.Constant):
            if isinstance(node.value, (int, float)):
                return node.value
            raise ValueError('Unsupported constant')

        # Variables/names
        if isinstance(node, ast.Name):
            if node.id in parameters:
                return parameters[node.id]
            raise ValueError(f'Unknown parameter: {node.id}')

        # Binary operations
        if isinstance(node, ast.BinOp):
            op_type = type(node.op)
            oper = {
                ast.Add: operator.add,
                ast.Sub: operator.sub,
                ast.Mult: operator.mul,
                ast.Div: operator.truediv,
            }.get(op_type)
            if not oper:
                raise ValueError(f'Cannot evaluate: {op_type.__name__}')
            left = _eval(node.left)
            right = _eval(node.right)
            return oper(left, right)

        raise ValueError(f'Unsupported expression: {type(node).__name__}')

    return _eval(ast.parse(value, mode='eval'))
