#pragma once
// FIX 4.4 tag numbers, message types and enum values used here, named as in the FIX spec.

#include <string_view>

namespace mt::fix {

namespace tag {
// Standard header and trailer
inline constexpr int BeginString = 8;
inline constexpr int BodyLength = 9;
inline constexpr int CheckSum = 10;
inline constexpr int MsgSeqNum = 34;
inline constexpr int MsgType = 35;
inline constexpr int PossDupFlag = 43;
inline constexpr int SenderCompID = 49;
inline constexpr int SendingTime = 52;
inline constexpr int TargetCompID = 56;
inline constexpr int OrigSendingTime = 122;

// Session-level messages
inline constexpr int BeginSeqNo = 7;
inline constexpr int EndSeqNo = 16;
inline constexpr int NewSeqNo = 36;
inline constexpr int RefSeqNum = 45;
inline constexpr int Text = 58;
inline constexpr int EncryptMethod = 98;
inline constexpr int HeartBtInt = 108;
inline constexpr int TestReqID = 112;
inline constexpr int GapFillFlag = 123;
inline constexpr int ResetSeqNumFlag = 141;
inline constexpr int RefMsgType = 372;
inline constexpr int BusinessRejectReason = 380;

// Orders and execution reports
inline constexpr int AvgPx = 6;
inline constexpr int ClOrdID = 11;
inline constexpr int CumQty = 14;
inline constexpr int ExecID = 17;
inline constexpr int ExecInst = 18;
inline constexpr int LastPx = 31;
inline constexpr int LastQty = 32;
inline constexpr int OrderID = 37;
inline constexpr int OrderQty = 38;
inline constexpr int OrdStatus = 39;
inline constexpr int OrdType = 40;
inline constexpr int OrigClOrdID = 41;
inline constexpr int Price = 44;
inline constexpr int Side = 54;
inline constexpr int Symbol = 55;
inline constexpr int TimeInForce = 59;
inline constexpr int TransactTime = 60;
inline constexpr int CxlRejReason = 102;
inline constexpr int OrdRejReason = 103;
inline constexpr int ExecType = 150;
inline constexpr int LeavesQty = 151;
inline constexpr int CxlRejResponseTo = 434;
inline constexpr int OrdStatusReqID = 790;
inline constexpr int LastLiquidityInd = 851;
}  // namespace tag

namespace msg_type {
// Session (admin) messages
inline constexpr std::string_view Heartbeat = "0";
inline constexpr std::string_view TestRequest = "1";
inline constexpr std::string_view ResendRequest = "2";
inline constexpr std::string_view Reject = "3";
inline constexpr std::string_view SequenceReset = "4";
inline constexpr std::string_view Logout = "5";
inline constexpr std::string_view Logon = "A";

// Application messages
inline constexpr std::string_view ExecutionReport = "8";
inline constexpr std::string_view OrderCancelReject = "9";
inline constexpr std::string_view NewOrderSingle = "D";
inline constexpr std::string_view OrderCancelRequest = "F";
inline constexpr std::string_view OrderCancelReplaceRequest = "G";
inline constexpr std::string_view OrderStatusRequest = "H";
inline constexpr std::string_view BusinessMessageReject = "j";
}  // namespace msg_type

// Admin messages are never resent: a ResendRequest covering one gets a SequenceReset-GapFill instead.
bool is_admin_message(std::string_view type);

namespace exec_type {  // ExecType(150)
inline constexpr char New = '0';
inline constexpr char Canceled = '4';
inline constexpr char Replaced = '5';
inline constexpr char Rejected = '8';
inline constexpr char Expired = 'C';
inline constexpr char Trade = 'F';
inline constexpr char OrderStatus = 'I';
}  // namespace exec_type

namespace ord_status {  // OrdStatus(39)
inline constexpr char New = '0';
inline constexpr char PartiallyFilled = '1';
inline constexpr char Filled = '2';
inline constexpr char Canceled = '4';
inline constexpr char Rejected = '8';
inline constexpr char Expired = 'C';
}  // namespace ord_status

namespace side_code {  // Side(54)
inline constexpr char Buy = '1';
inline constexpr char Sell = '2';
}  // namespace side_code

namespace ord_type {  // OrdType(40)
inline constexpr char Limit = '2';
}

namespace time_in_force {  // TimeInForce(59)
inline constexpr char Day = '0';
inline constexpr char GoodTillCancel = '1';
inline constexpr char ImmediateOrCancel = '3';
}  // namespace time_in_force

// ExecInst(18) '6' (participate, don't initiate) = post-only.
inline constexpr char kExecInstPostOnly = '6';

// LastLiquidityInd(851): added = maker, removed = taker.
inline constexpr char kLiquidityAdded = '1';
inline constexpr char kLiquidityRemoved = '2';

}  // namespace mt::fix
