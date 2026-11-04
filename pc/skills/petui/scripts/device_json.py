"""Device string representation only, not JSON grammar/schema/cycle validation."""
def _strings_valid(value,unicode_text):
    pending=[value];seen=set()
    while pending:
        item=pending.pop()
        if isinstance(item,str):
            if '\0' in item:return False
            if unicode_text:
                try:item.encode('utf-8')
                except UnicodeEncodeError:return False
        elif isinstance(item,(dict,list)):
            if id(item) in seen:continue
            seen.add(id(item))
            if isinstance(item,dict):
                pending.extend(item.keys());pending.extend(item.values())
            else:pending.extend(item)
    return True
def strings_no_nul(value):
    return _strings_valid(value,False)
def strings_valid_text(value):
    return _strings_valid(value,True)

DEVICE_JSON_MAX_DEPTH=32
def value_within_depth(value,maximum=DEVICE_JSON_MAX_DEPTH):
    if type(maximum)is not int or not 1<=maximum<=DEVICE_JSON_MAX_DEPTH:return False
    pending=[(value,1)];seen={}
    while pending:
        item,depth=pending.pop()
        if not isinstance(item,(dict,list)):continue
        if depth>maximum:return False
        identity=id(item)
        if seen.get(identity,0)>=depth:continue
        seen[identity]=depth
        children=item.values() if isinstance(item,dict) else item
        pending.extend((child,depth+1) for child in children)
    return True
