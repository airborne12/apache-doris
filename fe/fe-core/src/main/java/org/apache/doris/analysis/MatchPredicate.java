// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package org.apache.doris.analysis;

import org.apache.doris.catalog.ArrayType;
import org.apache.doris.catalog.Function;
import org.apache.doris.catalog.Function.NullableMode;
import org.apache.doris.catalog.FunctionSet;
import org.apache.doris.catalog.Index;
import org.apache.doris.catalog.ScalarFunction;
import org.apache.doris.catalog.TableIf;
import org.apache.doris.catalog.TableIf.TableType;
import org.apache.doris.catalog.Type;
import org.apache.doris.thrift.TExprNode;
import org.apache.doris.thrift.TExprNodeType;
import org.apache.doris.thrift.TExprOpcode;
import org.apache.doris.thrift.TMatchPredicate;

import com.google.common.base.Preconditions;
import com.google.common.collect.Lists;
import com.google.gson.annotations.SerializedName;

import com.google.common.base.Strings;

import java.util.Collections;
import java.util.Map;
import java.util.Objects;

/**
 * filed MATCH query_str
 */
public class MatchPredicate extends Predicate {

    public enum Operator {
        MATCH_ANY("MATCH_ANY", "match_any", TExprOpcode.MATCH_ANY),
        MATCH_ALL("MATCH_ALL", "match_all", TExprOpcode.MATCH_ALL),
        MATCH_PHRASE("MATCH_PHRASE", "match_phrase", TExprOpcode.MATCH_PHRASE),
        MATCH_PHRASE_PREFIX("MATCH_PHRASE_PREFIX", "match_phrase_prefix", TExprOpcode.MATCH_PHRASE_PREFIX),
        MATCH_REGEXP("MATCH_REGEXP", "match_regexp", TExprOpcode.MATCH_REGEXP),
        MATCH_PHRASE_EDGE("MATCH_PHRASE_EDGE", "match_phrase_edge", TExprOpcode.MATCH_PHRASE_EDGE);

        private final String description;
        private final String name;
        private final TExprOpcode opcode;

        Operator(String description,
                 String name,
                 TExprOpcode opcode) {
            this.description = description;
            this.name = name;
            this.opcode = opcode;
        }

        @Override
        public String toString() {
            return description;
        }

        public String getName() {
            return name;
        }

        public TExprOpcode getOpcode() {
            return opcode;
        }
    }

    public static void initBuiltins(FunctionSet functionSet) {
        String symbolNotUsed = "symbol_not_used";

        for (Type t : Type.getStringTypes()) {
            functionSet.addBuiltinBothScalaAndVectorized(ScalarFunction.createBuiltinOperator(
                    Operator.MATCH_ANY.getName(),
                    symbolNotUsed,
                    Lists.<Type>newArrayList(t, t),
                    Type.BOOLEAN));
            functionSet.addBuiltinBothScalaAndVectorized(ScalarFunction.createBuiltinOperator(
                    Operator.MATCH_ANY.getName(),
                    symbolNotUsed,
                    Lists.<Type>newArrayList(new ArrayType(t), t),
                    Type.BOOLEAN));

            functionSet.addBuiltinBothScalaAndVectorized(ScalarFunction.createBuiltinOperator(
                    Operator.MATCH_ALL.getName(),
                    symbolNotUsed,
                    Lists.<Type>newArrayList(t, t),
                    Type.BOOLEAN));
            functionSet.addBuiltinBothScalaAndVectorized(ScalarFunction.createBuiltinOperator(
                    Operator.MATCH_ALL.getName(),
                    symbolNotUsed,
                    Lists.<Type>newArrayList(new ArrayType(t), t),
                    Type.BOOLEAN));

            functionSet.addBuiltinBothScalaAndVectorized(ScalarFunction.createBuiltinOperator(
                    Operator.MATCH_PHRASE.getName(),
                    symbolNotUsed,
                    Lists.<Type>newArrayList(t, t),
                    Type.BOOLEAN));
            functionSet.addBuiltinBothScalaAndVectorized(ScalarFunction.createBuiltinOperator(
                    Operator.MATCH_PHRASE.getName(),
                    symbolNotUsed,
                    Lists.<Type>newArrayList(new ArrayType(t), t),
                    Type.BOOLEAN));
            functionSet.addBuiltinBothScalaAndVectorized(ScalarFunction.createBuiltinOperator(
                    Operator.MATCH_PHRASE_PREFIX.getName(),
                    symbolNotUsed,
                    Lists.<Type>newArrayList(t, t),
                    Type.BOOLEAN));
            functionSet.addBuiltinBothScalaAndVectorized(ScalarFunction.createBuiltinOperator(
                    Operator.MATCH_PHRASE_PREFIX.getName(),
                    symbolNotUsed,
                    Lists.<Type>newArrayList(new ArrayType(t), t),
                    Type.BOOLEAN));
            functionSet.addBuiltinBothScalaAndVectorized(ScalarFunction.createBuiltinOperator(
                    Operator.MATCH_REGEXP.getName(),
                    symbolNotUsed,
                    Lists.<Type>newArrayList(t, t),
                    Type.BOOLEAN));
            functionSet.addBuiltinBothScalaAndVectorized(ScalarFunction.createBuiltinOperator(
                    Operator.MATCH_REGEXP.getName(),
                    symbolNotUsed,
                    Lists.<Type>newArrayList(new ArrayType(t), t),
                    Type.BOOLEAN));
            functionSet.addBuiltinBothScalaAndVectorized(ScalarFunction.createBuiltinOperator(
                    Operator.MATCH_PHRASE_EDGE.getName(),
                    symbolNotUsed,
                    Lists.<Type>newArrayList(t, t),
                    Type.BOOLEAN));
            functionSet.addBuiltinBothScalaAndVectorized(ScalarFunction.createBuiltinOperator(
                    Operator.MATCH_PHRASE_EDGE.getName(),
                    symbolNotUsed,
                    Lists.<Type>newArrayList(new ArrayType(t), t),
                    Type.BOOLEAN));
        }
    }

    @SerializedName("op")
    private Operator op;
    private String selectedAnalyzer = "";
    private String selectedParser = "";
    private String explicitAnalyzer = "";

    private MatchPredicate() {
        // use for serde only
        selectedAnalyzer = "";
        selectedParser = "";
    }

    public MatchPredicate(Operator op, Expr e1, Expr e2) {
        super();
        this.op = op;
        Preconditions.checkNotNull(e1);
        children.add(e1);
        Preconditions.checkNotNull(e2);
        children.add(e2);
        // TODO: Calculate selectivity
        selectivity = Expr.DEFAULT_SELECTIVITY;
    }

    protected MatchPredicate(MatchPredicate other) {
        super(other);
        op = other.op;
        selectedAnalyzer = other.selectedAnalyzer;
        selectedParser = other.selectedParser;
        explicitAnalyzer = other.explicitAnalyzer;
    }

    /**
     * use for Nereids ONLY
     */
    public MatchPredicate(Operator op, Expr e1, Expr e2, Type retType,
            NullableMode nullableMode, Index invertedIndex) {
        this(op, e1, e2, retType, nullableMode, invertedIndex, null);
    }

    public MatchPredicate(Operator op, Expr e1, Expr e2, Type retType,
            NullableMode nullableMode, Index invertedIndex, String analyzer) {
        this(op, e1, e2);
        AnalyzerSelector.Selection selection = AnalyzerSelector.select(invertedIndex, analyzer);
        this.selectedAnalyzer = selection.analyzer();
        this.selectedParser = selection.parser();
        if (Strings.isNullOrEmpty(this.selectedAnalyzer)) {
            this.selectedAnalyzer = this.selectedParser;
        }
        if (!Strings.isNullOrEmpty(analyzer)) {
            this.explicitAnalyzer = analyzer.trim();
        }
        fn = new Function(new FunctionName(op.name), Lists.newArrayList(e1.getType(), e2.getType()), retType,
                false, true, nullableMode);
    }

    @Override
    public Expr clone() {
        return new MatchPredicate(this);
    }

    public Operator getOp() {
        return this.op;
    }

    @Override
    public boolean equals(Object obj) {
        if (!super.equals(obj)) {
            return false;
        }
        MatchPredicate other = (MatchPredicate) obj;
        return other.op == op
                && Objects.equals(explicitAnalyzer, other.explicitAnalyzer)
                && Objects.equals(selectedAnalyzer, other.selectedAnalyzer)
                && Objects.equals(selectedParser, other.selectedParser);
    }

    @Override
    public String toSqlImpl() {
        return getChild(0).toSql() + " " + op.toString() + " " + getChild(1).toSql()
                + analyzerSqlFragment();
    }

    @Override
    public String toSqlImpl(boolean disableTableName, boolean needExternalSql, TableType tableType,
            TableIf table) {
        return getChild(0).toSql(disableTableName, needExternalSql, tableType, table) + " " + op.toString() + " "
                + getChild(1).toSql(disableTableName, needExternalSql, tableType, table)
                + analyzerSqlFragment();
    }

    @Override
    protected void toThrift(TExprNode msg) {
        msg.node_type = TExprNodeType.MATCH_PRED;
        msg.setOpcode(op.getOpcode());
        TMatchPredicate matchPredicate = new TMatchPredicate();
        if (!Strings.isNullOrEmpty(selectedAnalyzer)) {
            matchPredicate.setAnalyzer(selectedAnalyzer);
        }
        if (!Strings.isNullOrEmpty(selectedParser)) {
            matchPredicate.setParser(selectedParser);
        }
        msg.match_predicate = matchPredicate;
    }

    @Override
    public int hashCode() {
        return Objects.hash(super.hashCode(), op, explicitAnalyzer, selectedAnalyzer, selectedParser);
    }

    private String analyzerSqlFragment() {
        if (explicitAnalyzer == null || explicitAnalyzer.isEmpty()) {
            return "";
        }
        if (explicitAnalyzer.matches("[A-Za-z_][A-Za-z0-9_]*")) {
            return " USING ANALYZER " + explicitAnalyzer;
        }
        return " USING ANALYZER '" + explicitAnalyzer.replace("'", "''") + "'";
    }
}
