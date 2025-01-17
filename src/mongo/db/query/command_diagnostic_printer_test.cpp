/**
 *    Copyright (C) 2024-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/command_diagnostic_printer.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/logv2/log_util.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {

namespace {

using namespace fmt::literals;

static constexpr StringData kCmdName = "mockCmd"_sd;
static constexpr StringData kCmdValue = "abcdefgh"_sd;
static constexpr StringData kSensitiveFieldName = "sensitive"_sd;
static constexpr StringData kSensitiveValue = "12345678"_sd;

class MockCmd : public BasicCommand {
public:
    MockCmd() : BasicCommand{kCmdName} {}

    std::set<StringData> sensitiveFieldNames() const final {
        return {kSensitiveFieldName};
    }

    bool run(OperationContext*, const DatabaseName&, const BSONObj&, BSONObjBuilder&) override {
        return true;
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    bool supportsWriteConcern(const BSONObj&) const override {
        return true;
    }
    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();
    }

    bool enableDiagnosticPrintingOnFailure() const override {
        return true;
    }
};

class CommandDiagnosticPrinterTest : public ServiceContextTest {
public:
    CommandDiagnosticPrinterTest() {
        _nss = NamespaceString::createNamespaceString_forTest("myDB.myColl");
        _opCtxHolder = makeOperationContext();
        _cmdBson = BSON(kCmdName << kCmdValue << kSensitiveFieldName << kSensitiveValue);
    }

    OperationContext* opCtx() {
        return _opCtxHolder.get();
    }

    CurOp* curOp() {
        return CurOp::get(opCtx());
    }

    void setMockCmdOnCurOp() {
        stdx::lock_guard<Client> clientLock(*opCtx()->getClient());
        curOp()->setGenericOpRequestDetails(clientLock, _nss, &_cmd, _cmdBson, NetworkOp::dbQuery);
    }

    void setCmdOnCurOp(Command* cmdObj, const BSONObj& cmdBson) {
        stdx::lock_guard<Client> clientLock(*opCtx()->getClient());
        curOp()->setGenericOpRequestDetails(clientLock, _nss, cmdObj, cmdBson, NetworkOp::dbQuery);
    }

    std::string printCommandDiagnostics() {
        command_diagnostics::Printer printer{opCtx()};
        return "{}"_format(printer);
    }

    MockCmd _cmd;
    NamespaceString _nss;
    ServiceContext::UniqueOperationContext _opCtxHolder;
    BSONObj _cmdBson;
};

TEST_F(CommandDiagnosticPrinterTest, PrinterOmitsCommandFieldsWhenThereIsNoCommandSet) {
    // When CurOp doesn't have a command object on it, the diagnostic printer shouldn't log any
    // command fields, since it's unclear if any of them are sensitive.
    ASSERT_EQ(command_diagnostics::Printer::kOmitUnrecognizedCommandMsg, printCommandDiagnostics());
}

TEST_F(CommandDiagnosticPrinterTest, PrinterOmitsAllFieldsWhenRequested) {
    // When a command requests to omit diagnostic logging, the diagnostic printer shouldn't log any
    // fields.
    setMockCmdOnCurOp();
    {
        stdx::lock_guard<Client> clientLock(*opCtx()->getClient());
        curOp()->setShouldOmitDiagnosticInformation(clientLock, true);
    }
    ASSERT_EQ(command_diagnostics::Printer::kOmitUnsupportedCurOpMsg, printCommandDiagnostics());
}

TEST_F(CommandDiagnosticPrinterTest, PrinterRedactsSensitiveCommandFields) {
    // The diagnostic printer should always redact the values of fields specified as sensitive by
    // the command.
    setMockCmdOnCurOp();
    auto str = printCommandDiagnostics();
    ASSERT_STRING_CONTAINS(str, kCmdName);
    ASSERT_STRING_CONTAINS(str, kCmdValue);
    ASSERT_STRING_CONTAINS(str, kSensitiveFieldName);
    ASSERT_STRING_OMITS(str, kSensitiveValue);
}

TEST_F(CommandDiagnosticPrinterTest, PrinterRedactsWhenRedactionIsEnabled) {
    // When redaction is enabled, all field values should be redacted.
    setMockCmdOnCurOp();
    logv2::setShouldRedactLogs(true);
    auto str = printCommandDiagnostics();
    ASSERT_STRING_CONTAINS(str, kCmdName);
    ASSERT_STRING_OMITS(str, kCmdValue);
    ASSERT_STRING_CONTAINS(str, kSensitiveFieldName);
    ASSERT_STRING_OMITS(str, kSensitiveValue);

    // Reset at the end of the test to not affect other test cases.
    logv2::setShouldRedactLogs(false);
}

TEST_F(CommandDiagnosticPrinterTest, OmitsAllFieldsWhenCommandDoesNotEnableDiagnosticPrinting) {
    class MockCmdWithoutDiagnosticPrinting : public MockCmd {
        bool enableDiagnosticPrintingOnFailure() const final {
            return false;
        }
    };

    MockCmdWithoutDiagnosticPrinting cmdWithoutPrinting;
    BSONObj mockBson = BSON("mockCmd" << 1);
    setCmdOnCurOp(&cmdWithoutPrinting, mockBson);
    ASSERT_EQ(command_diagnostics::Printer::kOmitUnsupportedCommandMsg, printCommandDiagnostics());
}

TEST_F(CommandDiagnosticPrinterTest, FormattingGracefullyExitsWhenOpCtxIsNull) {
    command_diagnostics::Printer printer{nullptr};
    ASSERT_EQ(command_diagnostics::Printer::kOpCtxIsNullMsg, "{}"_format(printer));
}

TEST_F(CommandDiagnosticPrinterTest, CreateIndexCommandIsEligibleForDiagnosticLog) {
    auto command = CommandHelpers::findCommand(opCtx(), "createIndexes");
    auto createIndexesReq =
        BSON("createIndexes" << _nss.coll() << "indexes"
                             << BSON_ARRAY(BSON("key" << BSON("a" << 1) << "partialFilterExpression"
                                                      << BSON("b" << 1))));

    // Prove that the command BSON is appropriate for this command (parsing succeeds).
    auto request = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::get(opCtx()), _nss.dbName(), createIndexesReq);
    ASSERT_NOT_EQUALS(command->parse(opCtx(), request), nullptr);

    // Diagnostics log includes the entire command BSON (command name, namespace, and index spec).
    setCmdOnCurOp(command, createIndexesReq);
    auto str = printCommandDiagnostics();
    ASSERT_STRING_CONTAINS(str, createIndexesReq.toString());
}
}  // namespace

}  // namespace mongo
