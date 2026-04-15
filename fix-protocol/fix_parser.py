import time

class FIXMessage:
    def __init__(self):
        self.fields = {}
    
    def parse(self, raw_msg):
        """Parse FIX message from raw string"""
        start = time.time_ns()
        pairs = raw_msg.split('|')
        for pair in pairs:
            if '=' in pair:
                tag, value = pair.split('=', 1)
                try:
                    tag_num = int(tag)
                except ValueError:
                    continue  # skip malformed tags
                if tag_num <= 0:
                    continue  # FIX tags are positive integers
                self.fields[tag_num] = value
        elapsed = time.time_ns() - start
        return elapsed
    
    def get_msg_type(self):
        return self.fields.get(35, 'UNKNOWN')
    
    def get_symbol(self):
        return self.fields.get(55, 'UNKNOWN')
    
    def get_side(self):
        side = self.fields.get(54, '0')
        return 'BUY' if side == '1' else 'SELL'
    
    def get_price(self):
        return float(self.fields.get(44, 0))
    
    def get_quantity(self):
        return int(self.fields.get(38, 0))
    
    def __str__(self):
        msg_type = self.get_msg_type()
        types = {'D': 'NEW ORDER', 'G': 'MODIFY', 'F': 'CANCEL',
                 '8': 'EXECUTION', '0': 'HEARTBEAT'}
        return f"{types.get(msg_type, msg_type)} | {self.get_side()} {self.get_quantity()} {self.get_symbol()} @ {self.get_price()}"


# FIX tag reference:
# 8  = BeginString
# 35 = MsgType (D=NewOrder, G=Modify, F=Cancel, 8=Execution)
# 49 = SenderCompID
# 56 = TargetCo
# 56 = TargetCompID
# 55 = Symbol
# 54 = Side (1=Buy, 2=Sell)
# 44 = Price
# 38 = OrderQty

sample_messages = [
    "8=FIX.4.2|35=D|49=TRADER1|56=EXCHANGE|55=AAPL|54=1|44=150.25|38=100",
    "8=FIX.4.2|35=D|49=TRADER1|56=EXCHANGE|55=MSFT|54=2|44=380.50|38=50",
    "8=FIX.4.2|35=F|49=TRADER1|56=EXCHANGE|55=AAPL|54=1|44=150.25|38=100",
    "8=FIX.4.2|35=8|49=EXCHANGE|56=TRADER1|55=AAPL|54=1|44=150.25|38=100",
]


def main():
    print("=== FIX Protocol Parser ===\n")
    total_ns = 0
    for raw in sample_messages:
        msg = FIXMessage()
        ns = msg.parse(raw)
        total_ns += ns
        print(f"  {msg}")
        print(f"  Parse time: {ns} ns\n")
    print(f"Average parse time: {total_ns // len(sample_messages)} ns")


if __name__ == '__main__':
    main()
