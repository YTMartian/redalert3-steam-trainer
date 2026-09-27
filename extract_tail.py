import re

raw = open(r'C:\Users\YTMartian\Desktop\redalert3\RedAlert3_Trainer_1.12_FINAL3.exe', 'rb').read()
tail = raw[0x8D000:]
out = []
for m in re.finditer(rb'[\x20-\x7e]{3,}', tail):
    out.append('0x%06x  %s' % (0x8D000 + m.start(), m.group().decode('ascii', 'replace')))
open(r'C:\Users\YTMartian\Desktop\redalert3\tail_strings.txt', 'w', encoding='utf-8').write('\n'.join(out))
print('lines', len(out))
print('tail size', hex(len(tail)))
