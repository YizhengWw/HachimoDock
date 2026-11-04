"""Bounded content projection shared by simulator and structural tests."""
def viewports(surface):
    return surface['viewports']['items'] if 'viewports' in surface else [surface['viewport']] if 'viewport' in surface else []
def node_viewport(surface,node):
    return next((v for v in surface.get('viewports',{}).get('items',[]) if v['id']==node['view']),None) if 'view' in node else surface.get('viewport') if node.get('in_view') else None
def offset(surface,variables,view=None):
    v=view if view is not None else surface.get('viewport')
    if not v:return 0
    value=variables.get(v['offset_var'],0)
    return max(0,min(v['content_height']-v['h'],value if type(value)is int else 0))
def project(surface,node,variables):
    if not node.get('in_view') and 'view' not in node:return node
    v=node_viewport(surface,node)
    if not v:return None
    n={**node,'x':node['x']+v['x'],'y':node['y']+v['y']-offset(surface,variables,v)}
    return n if n['x']<v['x']+v['w'] and n['x']+n['w']>v['x'] and n['y']<v['y']+v['h'] and n['y']+n['h']>v['y'] else None
def hit(surface,node,variables,x,y):
    n=project(surface,node,variables);v=node_viewport(surface,node)
    return bool(n and (v is None or v['x']<=x<v['x']+v['w'] and v['y']<=y<v['y']+v['h']) and n['x']<=x<n['x']+n['w'] and n['y']<=y<n['y']+n['h'])
